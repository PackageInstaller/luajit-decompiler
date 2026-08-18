#include "../main.h"

// 各 pass 之间共享的内部辅助函数 (声明; 定义在 ast/eliminate.cpp)。
// 注意: 本头文件不包含 main.h, 引用前必须已包含之。


#include "ast_internal.h"

void Ast::eliminate_slots(Function& function, std::vector<Statement*>& block, BlockInfo* const& previousBlock) {
	std::unordered_map<const SlotScope*, uint32_t> refCounts;
	collect_scope_reads(function, refCounts);
	std::unordered_set<const SlotScope*> writtenScopes;
	collect_written_scopes(function, writtenScopes);
	eliminate_slots(function, block, previousBlock, refCounts, writtenScopes);
}

void Ast::eliminate_slots(Function& function, std::vector<Statement*>& block, BlockInfo* const& previousBlock, std::unordered_map<const SlotScope*, uint32_t>& refCounts, std::unordered_set<const SlotScope*>& writtenScopes) {
	BlockInfo blockInfo = { .block = block, .previousBlock = previousBlock };
	Expression* expression;
	uint32_t index, targetIndex, targetLabel, extendedTargetLabel;
	bool hasBoolConstruct;

	for (uint32_t i = 0; i < block.size(); i++) {
		switch (block[i]->type) {
		case AST_STATEMENT_CONDITION:
			if (block[i]->condition.allowSlotSwap
				&& i
				&& !function.is_valid_label(block[i]->instruction.label)
				&& block[i - 1]->type == AST_STATEMENT_ASSIGNMENT
				&& block[i - 1]->assignment.variables.size() == 1
				&& block[i - 1]->assignment.variables.back().type == AST_VARIABLE_SLOT
				&& (*block[i - 1]->assignment.variables.back().slotScope)->usages == 1
				&& block[i - 1]->assignment.variables.back().slot == block[i]->assignment.expressions[0]->variable->slot) {
				expression = block[i]->assignment.expressions[0];
				block[i]->assignment.expressions[0] = block[i]->assignment.expressions[1];
				block[i]->assignment.expressions[1] = expression;
				block[i]->condition.swapped = true;
			}

			break;
		case AST_STATEMENT_GENERIC_FOR:
		case AST_STATEMENT_DECLARATION:
			while (i && !function.is_valid_label(block[i]->instruction.label)) {
				switch (block[i - 1]->type) {
				case AST_STATEMENT_ASSIGNMENT:
					if (!block[i]->assignment.openSlots.size()) break;
					if (block[i - 1]->assignment.variables.front().slot <= block[i]->assignment.expressions[block[i]->assignment.openSlots.size() - 1]->variable->slot) break;
					assert(block[i - 1]->assignment.variables.size() == 1 && !(*block[i - 1]->assignment.variables.back().slotScope)->usages, "Invalid expression list assignment", bytecode.filePath, DEBUG_INFO);
				case AST_STATEMENT_FUNCTION_CALL:
					block[i]->assignment.expressions.emplace(block[i]->assignment.expressions.begin() + block[i]->assignment.openSlots.size(), block[i - 1]->assignment.expressions.back());
					block[i]->instruction.label = block[i - 1]->instruction.label;
					i--;
					block.erase(block.begin() + i);
					continue;
				}

				if (block[i - 1]->type == AST_STATEMENT_ASSIGNMENT && block[i - 1]->assignment.variables.size() != 1) {
					assert(block[i]->assignment.expressions.size() == block[i]->assignment.openSlots.size()
						&& block[i]->assignment.expressions.back()->variable->slot == block[i - 1]->assignment.variables.back().slot,
						"Invalid multres expression list assignment", bytecode.filePath, DEBUG_INFO);

					while (true) {
						function.slotScopeCollector.remove_scope(block[i]->assignment.expressions.back()->variable->slot, block[i]->assignment.expressions.back()->variable->slotScope);
						block[i]->assignment.openSlots.pop_back();

						if (block[i]->assignment.expressions.back()->variable->slot != block[i - 1]->assignment.variables.front().slot) {
							block[i]->assignment.expressions.pop_back();
							continue;
						}

						block[i]->assignment.expressions.back() = block[i - 1]->assignment.expressions.back();
						block[i]->instruction.label = block[i - 1]->instruction.label;
						i--;
						block.erase(block.begin() + i);
						break;
					}
				}

				for (uint32_t j = block[i]->assignment.openSlots.size(); j--;) {
					block[i]->assignment.openSlots[j] = &block[i]->assignment.expressions[j];
				}

				break;
			}

			if (block[i]->type == AST_STATEMENT_DECLARATION) goto eliminate_function_call_open_slots;
			break;
		case AST_STATEMENT_ASSIGNMENT:
		case AST_STATEMENT_FUNCTION_CALL:
			if (block[i]->type == AST_STATEMENT_FUNCTION_CALL) {
				// FUNCTION_CALL(无返回值调用) 语句: 无赋值目标, 直接进入 openSlots 消除。
				goto eliminate_function_call_open_slots;
			}
			switch (block[i]->assignment.variables.back().type) {
			case AST_VARIABLE_SLOT:
				if (block[i]->assignment.expressions.back()->type == AST_EXPRESSION_BINARY_OPERATION
					&& block[i]->assignment.expressions.back()->binaryOperation->type != AST_BINARY_CONCATENATION
					&& block[i]->assignment.openSlots.size() == 2
					&& i >= 2
					&& !function.is_valid_label(block[i]->instruction.label)
					&& !function.is_valid_label(block[i - 1]->instruction.label)
					&& block[i - 1]->type == AST_STATEMENT_ASSIGNMENT
					&& block[i - 1]->assignment.variables.size() == 1
					&& block[i - 1]->assignment.variables.back().type == AST_VARIABLE_SLOT
					&& (*block[i - 1]->assignment.variables.back().slotScope)->usages == 1
					&& block[i - 1]->assignment.variables.back().slot == block[i]->assignment.expressions.back()->binaryOperation->leftOperand->variable->slot
					&& get_constant_type(block[i - 1]->assignment.expressions.back()) == NUMBER_CONSTANT
					&& (block[i - 2]->type == AST_STATEMENT_ASSIGNMENT
						|| (block[i - 2]->type == AST_STATEMENT_DECLARATION
							&& (*block[i - 2]->assignment.variables.back().slotScope)->isSynthetic))
					&& block[i - 2]->assignment.variables.size() == 1
					&& block[i - 2]->assignment.variables.back().type == AST_VARIABLE_SLOT
					&& (*block[i - 2]->assignment.variables.back().slotScope)->usages == 1
					&& block[i - 2]->assignment.variables.back().slot == block[i]->assignment.expressions.back()->binaryOperation->rightOperand->variable->slot) {
					block[i]->assignment.openSlots[0] = &block[i]->assignment.expressions.back()->binaryOperation->rightOperand;
					block[i]->assignment.openSlots[1] = &block[i]->assignment.expressions.back()->binaryOperation->leftOperand;
				}

				break;
			case AST_VARIABLE_TABLE_INDEX:
				if (!block[i]->assignment.variables.back().isMultres
					&& block[i]->assignment.variables.back().tableIndex->type == AST_EXPRESSION_VARIABLE
					&& i >= 3
					&& !function.is_valid_label(block[i]->instruction.label)
					&& !function.is_valid_label(block[i - 1]->instruction.label)
					&& !function.is_valid_label(block[i - 2]->instruction.label)
					&& block[i - 1]->type == AST_STATEMENT_ASSIGNMENT
					&& block[i - 1]->assignment.variables.size() == 1
					&& block[i - 1]->assignment.variables.back().type == AST_VARIABLE_SLOT
					&& (*block[i - 1]->assignment.variables.back().slotScope)->usages == 1
					&& block[i - 1]->assignment.variables.back().slot == block[i]->assignment.variables.back().tableIndex->variable->slot
					&& get_constant_type(block[i - 1]->assignment.expressions.back())
					&& block[i - 2]->type == AST_STATEMENT_ASSIGNMENT
					&& block[i - 2]->assignment.variables.size() == 1
					&& block[i - 2]->assignment.variables.back().type == AST_VARIABLE_SLOT
					&& (*block[i - 2]->assignment.variables.back().slotScope)->usages == 1
					&& block[i - 2]->assignment.variables.back().slot == block[i]->assignment.expressions.back()->variable->slot
					&& (!get_constant_type(block[i - 2]->assignment.expressions.back())
						|| get_constant_type(block[i - 1]->assignment.expressions.back()) == NIL_CONSTANT)
					&& block[i - 3]->assignment.isTableConstructor
					&& block[i - 3]->assignment.variables.back().slot == block[i]->assignment.variables.back().table->variable->slot
					&& !block[i - 3]->assignment.expressions.back()->table->multresField) {
					block[i]->assignment.openSlots[0] = &block[i]->assignment.expressions.back();
					block[i]->assignment.openSlots[1] = &block[i]->assignment.variables.back().tableIndex;
				}

				break;
			}

			break;
		}

	eliminate_function_call_open_slots:
		if (block[i]->type == AST_STATEMENT_DECLARATION
			&& block[i]->assignment.openSlots.size() == 1
			&& (*(*block[i]->assignment.openSlots.back())->variable->slotScope)->usages > 1
			&& i
			&& block[i - 1]->type == AST_STATEMENT_ASSIGNMENT
			&& block[i - 1]->assignment.variables.size() == 1
			&& block[i - 1]->assignment.variables.back().type == AST_VARIABLE_SLOT
			&& block[i - 1]->function
			&& block[i - 1]->function->assignmentSlotIsUpvalue
			&& block[i - 1]->assignment.variables.back().slot == (*block[i]->assignment.openSlots.back())->variable->slot) {
			if (SlotScope* openScope = *(*block[i]->assignment.openSlots.back())->variable->slotScope) refCounts[openScope]--;
			*block[i]->assignment.openSlots.back() = block[i - 1]->assignment.expressions.back();
			*block[i - 1]->assignment.variables.back().slotScope = *block[i]->assignment.variables.back().slotScope;
			block[i]->instruction.label = block[i - 1]->instruction.label;
			i--;
			function.slotScopeCollector.remove_scope(block[i]->assignment.variables.back().slot, block[i]->assignment.variables.back().slotScope);
			block.erase(block.begin() + i);
		} else {
			// 广义临时槽消除: 使用点前连续的单用途槽赋值链整体内联。
			//   local var_0 = table.insert
			//   local var_1 = fragment
			//   var_0(var_1, ...)
			// -> table.insert(fragment, ...)
			if (i && !function.is_valid_label(block[i]->instruction.label) && block[i]->assignment.openSlots.size()) {
				// 按槽位收集 open slot (沿用原逻辑, 只处理原始开放槽, 不做递归内联)。
				std::unordered_map<uint8_t, std::vector<Expression**>> openSlotsBySlot;
				std::unordered_map<Expression**, uint8_t> openSlotIndex;

				for (uint8_t j = 0; j < block[i]->assignment.openSlots.size(); j++) {
					Expression* slotExpression = *block[i]->assignment.openSlots[j];
					if (!slotExpression || slotExpression->type != AST_EXPRESSION_VARIABLE
						|| slotExpression->variable->type != AST_VARIABLE_SLOT)
						continue;
					openSlotsBySlot[slotExpression->variable->slot].emplace_back(block[i]->assignment.openSlots[j]);
					openSlotIndex[block[i]->assignment.openSlots[j]] = j;
				}

				// 向后扫描连续窗口: 允许 GOTO/EMPTY 与单变量 SLOT 赋值。
				// 限制扫描深度, 避免超长连续赋值链导致 O(n^2)。
				constexpr uint32_t MAX_WINDOW = 64;
				std::vector<uint32_t> window;
				for (uint32_t k = i; k-- > 0 && window.size() < MAX_WINDOW;) {
					Statement* previous = block[k];
					if (previous->type == AST_STATEMENT_GOTO || previous->type == AST_STATEMENT_EMPTY) {
						if (function.is_valid_label(previous->instruction.label)) break;
						window.emplace_back(k);
						continue;
					}
					if ((previous->type == AST_STATEMENT_ASSIGNMENT
							|| (previous->type == AST_STATEMENT_DECLARATION
								&& (*previous->assignment.variables.back().slotScope)->isSynthetic))
						&& previous->assignment.variables.size() == 1
						&& previous->assignment.expressions.size() == 1
						&& previous->assignment.variables.back().type == AST_VARIABLE_SLOT) {
						window.emplace_back(k);
						// 带跳转标签的赋值只允许作为整条链的起点: 跳入后从起点顺序执行,
						// 内联到使用点后语义不变; 标签落在链中间则跳过前置赋值, 不安全。
						if (function.is_valid_label(previous->instruction.label)) break;
						continue;
					}
					break;
				}

				std::unordered_set<uint8_t> matchedSlots;
				std::unordered_set<uint32_t> eraseIndices;

				for (uint32_t candidateIndex : window) {
					Statement* candidate = block[candidateIndex];
					if (candidate->type != AST_STATEMENT_ASSIGNMENT && candidate->type != AST_STATEMENT_DECLARATION) continue;

					const uint8_t slot = candidate->assignment.variables.back().slot;
					auto openSlotIt = openSlotsBySlot.find(slot);
					if (openSlotIt == openSlotsBySlot.end() || matchedSlots.contains(slot)) continue;
					// 同一槽被使用点引用多次时, 内联会重复求值, 保留临时变量。
					if (openSlotIt->second.size() != 1) continue;

					Expression* rhs = candidate->assignment.expressions.back();
					SlotScope* slotScope = *candidate->assignment.variables.back().slotScope;

					// 多返回值位置只能内联多返回值调用; 反过来单值位置允许内联多返回值调用
					// (多余返回值被丢弃, 与原 `local x = f(); g(x)` 语义一致)。
					bool multresUse = false;
					for (Expression** openSlot : openSlotIt->second) {
						if ((*openSlot)->variable->isMultres) multresUse = true;
					}
					if (multresUse && !candidate->assignment.variables.back().isMultres) continue;

					// RHS 若是槽位引用, 要求其作用域最终会被命名 (已有名字或存在写入语句),
					// 否则内联会留下 `var_<槽位>` 兜底名 (写入语句被合并/消除的幽灵作用域)。
					if (rhs->type == AST_EXPRESSION_VARIABLE && rhs->variable->type == AST_VARIABLE_SLOT) {
						SlotScope* rhsScope = *rhs->variable->slotScope;
						const uint8_t rhsSlot = rhs->variable->slot;
						const bool rhsIsParameter = rhsSlot < function.slotScopeCollector.slotInfos.size()
							&& function.slotScopeCollector.slotInfos[rhsSlot].isParameter;
						if (!rhsScope || (!rhsIsParameter && !writtenScopes.contains(rhsScope))) continue;
					}

					// 自引用或依赖窗口内其它将被内联的槽 -> 跳过, 保持求值顺序。
					bool unsafe = expression_references_scope(rhs, slotScope);
					for (uint32_t otherIndex : window) {
						if (unsafe) break;
						Statement* other = block[otherIndex];
						if (other->type != AST_STATEMENT_ASSIGNMENT && other->type != AST_STATEMENT_DECLARATION) continue;
						const uint8_t otherSlot = other->assignment.variables.back().slot;
						if (otherSlot == slot || !openSlotsBySlot.contains(otherSlot)) continue;
						if (expression_references_slot(rhs, otherSlot)) unsafe = true;
					}
					if (unsafe) continue;

					// 函数位置的常量类型限制 (沿用原逻辑)。
					bool isFunctionPosition = false;
					for (Expression** openSlot : openSlotIt->second) {
						if (openSlotIndex[openSlot] == 0) isFunctionPosition = true;
					}
					if (isFunctionPosition
						&& block[i]->assignment.allowedConstantType != NUMBER_CONSTANT
						&& get_constant_type(rhs) > block[i]->assignment.allowedConstantType)
						continue;

					// DECLARATION 的开放槽是局部变量初始化占位符: 若写入语句与声明变量
					// 是同一作用域, 这是 `local x = <rhs>` 的初始化合并, 即使 x 后续
					// 还有引用也应删除写入语句 (引用统一落到声明变量上)。
					bool mergeIntoDeclaration = false;
					if (block[i]->type == AST_STATEMENT_DECLARATION) {
						for (const Variable& variable : block[i]->assignment.variables) {
							if (variable.type == AST_VARIABLE_SLOT
								&& variable.slotScope
								&& *variable.slotScope == slotScope) {
								mergeIntoDeclaration = true;
								break;
							}
						}
					}

					// 只有写入语句会被删除 (无剩余引用/初始化合并) 时才允许内联
					// 带副作用的 RHS; 否则多用途临时槽的内联会把调用复制到使用点,
					// 导致重复执行。
					const bool willEraseWriter = mergeIntoDeclaration || refCounts[slotScope] == 1;
					if (!willEraseWriter && expression_has_side_effects(rhs)) continue;

					for (Expression** openSlot : openSlotIt->second) {
						// 递减的是开放槽实际引用的作用域计数 (可能与写入语句作用域不同)。
						SlotScope* openSlotScope = *(*openSlot)->variable->slotScope;
						if (openSlotScope) refCounts[openSlotScope]--;
						*openSlot = rhs;
					}
					matchedSlots.insert(slot);

					if (willEraseWriter) eraseIndices.insert(candidateIndex);
				}

				if (eraseIndices.size()) {
					std::vector<uint32_t> sortedErases(eraseIndices.begin(), eraseIndices.end());
					std::sort(sortedErases.begin(), sortedErases.end(), std::greater<>());

					for (uint32_t k : sortedErases) {
						function.slotScopeCollector.remove_scope(block[k]->assignment.variables.back().slot, block[k]->assignment.variables.back().slotScope);
						block[i]->instruction.label = block[k]->instruction.label;
						block.erase(block.begin() + k);
						if (k < i) i--;
					}
				}
			}
		}

		if (block[i]->assignment.openSlots.size()
			&& (*block[i]->assignment.openSlots.back())->type == AST_EXPRESSION_VARIABLE
			&& (*block[i]->assignment.openSlots.back())->variable->isMultres) {
			// 某些字节码变体的 TSETM 值可能不是多返回值调用 (如直接写数字常量),
			// 无法消除时保留槽位引用, 由写出阶段输出 `t[k] = 槽值`。
			if (SlotScope* openScope = *(*block[i]->assignment.openSlots.back())->variable->slotScope) refCounts[openScope]--;
			block[i]->assignment.openSlots.pop_back();
		}

		switch (block[i]->type) {
		case AST_STATEMENT_NUMERIC_FOR:
		case AST_STATEMENT_GENERIC_FOR:
			eliminate_slots(function, block[i]->block, nullptr, refCounts, writtenScopes);
			break;
		case AST_STATEMENT_LOOP:
		case AST_STATEMENT_DECLARATION:
			blockInfo.index = i;
			eliminate_slots(function, block[i]->block, &blockInfo, refCounts, writtenScopes);
			break;
		case AST_STATEMENT_ASSIGNMENT:
			if (block[i]->assignment.variables.size() == 1) {
				switch (block[i]->assignment.variables.back().type) {
				case AST_VARIABLE_SLOT:
					if (block[i]->instruction.id == INVALID_ID) break;
					blockInfo.index = i;
					targetLabel = get_label_from_next_statement(function, blockInfo, false, true);
					extendedTargetLabel = get_label_from_next_statement(function, blockInfo, true, true);
					if (!function.is_valid_label(targetLabel) || function.labels[targetLabel].jumpIds.front() > block[i]->instruction.id) break;

					if ((*block[i]->assignment.variables.back().slotScope)->usages >= 2) {
						if ((*block[i]->assignment.variables.back().slotScope)->scopeBegin >= function.labels[targetLabel].jumpIds.front()
							|| (extendedTargetLabel != targetLabel
								&& (function.labels[extendedTargetLabel].target <= block[i]->instruction.id
									|| function.labels[extendedTargetLabel].target >= function.labels[targetLabel].jumpIds.front()))
							|| has_self_reference(block[i]->assignment.variables.back().slot, block[i]->assignment.expressions.back()))
							break;
						index = get_block_index_from_id(block, function.labels[targetLabel].jumpIds.front() - 1);
						if (index == INVALID_ID) break;

						switch (block[index]->type) {
						case AST_STATEMENT_CONDITION:
							if (block[index]->assignment.variables.size()) {
								if ((*block[index]->assignment.variables.back().slotScope)->scopeBegin == block[index]->instruction.id
									&& *block[index]->assignment.variables.back().slotScope == *block[i]->assignment.variables.back().slotScope)
									break;
							} else if (index
									&& block[index]->assignment.expressions.size() == 1
									&& !function.is_valid_label(block[index]->instruction.label)) {
								if (block[index]->instruction.type == Bytecode::BC_OP_IST
									|| block[index]->instruction.type == Bytecode::BC_OP_ISF) {
									break;
								}
								if (block[index - 1]->type == AST_STATEMENT_ASSIGNMENT
									&& block[index - 1]->assignment.variables.size() == 1
									&& block[index - 1]->assignment.variables.back().type == AST_VARIABLE_SLOT
									&& (*block[index - 1]->assignment.variables.back().slotScope)->scopeBegin == block[index - 1]->instruction.id
									&& *block[index - 1]->assignment.variables.back().slotScope == *block[i]->assignment.variables.back().slotScope) {
									break;
								}
							}

							index = INVALID_ID;
							break;
						case AST_STATEMENT_ASSIGNMENT:
							if (block[index]->assignment.variables.size() != 1
								|| block[index]->assignment.variables.back().type != AST_VARIABLE_SLOT
								|| (*block[index]->assignment.variables.back().slotScope)->scopeBegin != block[index]->instruction.id
								|| *block[index]->assignment.variables.back().slotScope != *block[i]->assignment.variables.back().slotScope
								|| block[index]->assignment.expressions.size() != 1)
								index = INVALID_ID;
							break;
						}

						if (index == INVALID_ID) break;
						hasBoolConstruct = false;

						if (i >= 3
							&& block[i]->type == AST_STATEMENT_ASSIGNMENT
							&& block[i]->assignment.expressions.back()->type == AST_EXPRESSION_CONSTANT
							&& block[i]->assignment.expressions.back()->constant->type == AST_CONSTANT_TRUE
							&& (block[i - 1]->type == AST_STATEMENT_GOTO
								|| block[i - 1]->type == AST_STATEMENT_BREAK)
							&& !function.is_valid_label(block[i - 1]->instruction.label)
							&& block[i - 1]->instruction.type == Bytecode::BC_OP_JMP
							&& block[i - 1]->instruction.target == function.labels[targetLabel].target
							&& block[i - 2]->type == AST_STATEMENT_ASSIGNMENT
							&& block[i - 2]->assignment.expressions.back()->type == AST_EXPRESSION_CONSTANT
							&& block[i - 2]->assignment.expressions.back()->constant->type == AST_CONSTANT_FALSE
							&& block[i - 2]->assignment.variables.size() == 1
							&& block[i - 2]->assignment.variables.back().type == AST_VARIABLE_SLOT
							&& *block[i - 2]->assignment.variables.back().slotScope == *block[i]->assignment.variables.back().slotScope) {
							switch (block[i - 3]->type) {
							case AST_STATEMENT_CONDITION:
								if (block[i - 3]->assignment.expressions.size() == 2 && block[i - 3]->instruction.target == block[i]->instruction.id) hasBoolConstruct = true;
								break;
							case AST_STATEMENT_GOTO:
							case AST_STATEMENT_BREAK:
								if (i < 5
									|| function.is_valid_label(block[i - 3]->instruction.label)
									|| block[i - 3]->instruction.type != Bytecode::BC_OP_JMP
									|| block[i - 3]->instruction.target != function.labels[extendedTargetLabel].target
									|| (!function.is_valid_label(block[i]->instruction.label)
										&& !function.is_valid_label(block[i - 2]->instruction.label))
									|| block[i - 4]->type != AST_STATEMENT_ASSIGNMENT
									|| block[i - 4]->assignment.variables.size() != 1
									|| block[i - 4]->assignment.variables.back().type != AST_VARIABLE_SLOT
									|| block[i - 4]->assignment.variables.back().slot != block[i]->assignment.variables.back().slot)
									break;

								if (index == i - 2 && !function.is_valid_label(block[i]->instruction.label)) {
									if (function.labels[block[i - 2]->instruction.label].jumpIds.front() > block[i - 2]->instruction.id) break;
									index = get_block_index_from_id(block, function.labels[block[i - 2]->instruction.label].jumpIds.front() - 1);

									if (index == INVALID_ID) {
										index = i - 2;
										break;
									}
								}

								hasBoolConstruct = true;
								break;
							}

							if (hasBoolConstruct) {
								if ((function.is_valid_label(block[i]->instruction.label)
									&& function.labels[block[i]->instruction.label].jumpIds.back() >= block[i]->instruction.id)
									|| (function.is_valid_label(block[i - 2]->instruction.label)
										&& function.labels[block[i - 2]->instruction.label].jumpIds.back() >= block[i - 2]->instruction.id))
									break;

								if (function.is_valid_label(block[i]->instruction.label)) {
									for (uint32_t j = function.labels[block[i]->instruction.label].jumpIds.size(); j--;) {
										targetIndex = get_block_index_from_id(block, function.labels[block[i]->instruction.label].jumpIds[j] - 1);

										if (targetIndex == INVALID_ID
											|| block[targetIndex]->type != AST_STATEMENT_CONDITION
											|| block[targetIndex]->assignment.variables.size()) {
											index = INVALID_ID;
											break;
										}

										if (!block[targetIndex]->assignment.expressions.size()) {
											hasBoolConstruct = false;
											break;
										}
									}
								}

								if (hasBoolConstruct && function.is_valid_label(block[i - 2]->instruction.label)) {
									for (uint32_t j = function.labels[block[i - 2]->instruction.label].jumpIds.size(); j--;) {
										targetIndex = get_block_index_from_id(block, function.labels[block[i - 2]->instruction.label].jumpIds[j] - 1);

										if (targetIndex == INVALID_ID || block[targetIndex]->type != AST_STATEMENT_CONDITION) {
											index = INVALID_ID;
											break;
										}

										if (!block[targetIndex]->assignment.expressions.size() || block[targetIndex]->assignment.variables.size()) {
											hasBoolConstruct = false;
											break;
										}
									}
								}

								if (index == INVALID_ID) break;
							}
						}

						for (uint32_t j = i; index != INVALID_ID && block[index]->instruction.id < block[j]->instruction.id; j--) {
							if (function.is_valid_label(block[j]->instruction.label)) {
								if (function.labels[block[j]->instruction.label].jumpIds.back() >= block[j]->instruction.id) {
									index = INVALID_ID;
									break;
								}

								while (function.labels[block[j]->instruction.label].jumpIds.front() < block[index]->instruction.id) {
									if (!index) {
										index = INVALID_ID;
										break;
									}

									index--;
								}
							}
						}

						if (index != INVALID_ID) {
							switch (block[index]->type) {
							case AST_STATEMENT_CONDITION:
								if (block[index]->assignment.variables.size()) break;
							case AST_STATEMENT_GOTO:
							case AST_STATEMENT_BREAK:
								if (block[index]->instruction.target == function.labels[targetLabel].target && index) index--;
							}

							ConditionBuilder conditionBuilder(ConditionBuilder::ASSIGNMENT, *this, targetLabel,
								hasBoolConstruct ? block[i]->instruction.label : INVALID_ID, hasBoolConstruct ? block[i - 2]->instruction.label : INVALID_ID);
							targetIndex = hasBoolConstruct ? (block[i - 3]->type == AST_STATEMENT_GOTO ? i - 4 : i - 2) : i;

							for (uint32_t j = index; j < targetIndex; j++) {
								switch (block[j]->type) {
								case AST_STATEMENT_CONDITION:
									if (block[j]->instruction.target <= block[j]->instruction.id
										|| block[j]->instruction.target > function.labels[targetLabel].target
										|| (block[j]->instruction.target == function.labels[targetLabel].target
											? (!block[j]->assignment.variables.size()
												&& block[j]->instruction.type != Bytecode::BC_OP_IST
												&& block[j]->instruction.type != Bytecode::BC_OP_ISF)
												|| (block[j]->assignment.variables.size()
													&& *block[j]->assignment.variables.back().slotScope != *block[i]->assignment.variables.back().slotScope)
												|| has_self_reference(block[i]->assignment.variables.back().slot, block[j]->assignment.expressions.back())
											: block[j]->assignment.variables.size()))
										break;
									conditionBuilder.add_node(conditionBuilder.get_node_type(block[j]->instruction.type, block[j]->condition.swapped), block[j]->instruction.label,
										function.get_label_from_id(block[j]->instruction.target), &block[j]->assignment.expressions);
									continue;
								case AST_STATEMENT_ASSIGNMENT:
									if (block[j]->assignment.variables.size() != 1
										|| block[j]->assignment.variables.back().type != AST_VARIABLE_SLOT
										|| *block[j]->assignment.variables.back().slotScope != *block[i]->assignment.variables.back().slotScope
										|| has_self_reference(block[i]->assignment.variables.back().slot, block[j]->assignment.expressions.back())
										|| j + 1 == targetIndex
										|| function.is_valid_label(block[j + 1]->instruction.label))
										break;
									j++;

									switch (block[j]->type) {
									case AST_STATEMENT_CONDITION:
										if (block[j]->instruction.target != function.labels[targetLabel].target
											|| block[j]->assignment.variables.size()
											|| block[j]->assignment.expressions.size() != 1
											|| block[j]->assignment.expressions.back()->type != AST_EXPRESSION_VARIABLE
											|| block[j]->assignment.expressions.back()->variable->type != AST_VARIABLE_SLOT
											|| *block[j]->assignment.expressions.back()->variable->slotScope != *block[i]->assignment.variables.back().slotScope)
											break;
										conditionBuilder.add_node(conditionBuilder.get_node_type(block[j]->instruction.type, block[j]->condition.swapped), block[j - 1]->instruction.label,
											function.get_label_from_id(block[j]->instruction.target), &block[j - 1]->assignment.expressions);
										continue;
									case AST_STATEMENT_GOTO:
									case AST_STATEMENT_BREAK:
										if (function.is_valid_label(block[j]->instruction.label)
											|| block[j]->instruction.type != Bytecode::BC_OP_JMP
											|| block[j]->instruction.target != function.labels[targetLabel].target
											|| block[j - 1]->assignment.expressions.size() != 1)
											break;

										if (block[j - 1]->assignment.expressions.back()->type == AST_EXPRESSION_CONSTANT
											&& (block[j - 1]->assignment.expressions.back()->constant->type == AST_CONSTANT_NIL
												|| block[j - 1]->assignment.expressions.back()->constant->type == AST_CONSTANT_FALSE)) {
											conditionBuilder.add_node(ConditionBuilder::Node::FALSY_TEST, block[j - 1]->instruction.label,
												function.get_label_from_id(block[j]->instruction.target), &block[j - 1]->assignment.expressions);
										} else {
											conditionBuilder.add_node(ConditionBuilder::Node::TRUTHY_TEST, block[j - 1]->instruction.label,
												function.get_label_from_id(block[j]->instruction.target), &block[j - 1]->assignment.expressions);
										}

										continue;
									}

									break;
								}

								index = INVALID_ID;
								break;
							}

							if (!hasBoolConstruct) {
								conditionBuilder.add_node(ConditionBuilder::Node::TRUTHY_TEST, block[i]->instruction.label, targetLabel, &block[i]->assignment.expressions);
							} else if (block[i - 3]->type == AST_STATEMENT_GOTO) {
								conditionBuilder.add_node(ConditionBuilder::Node::TRUTHY_TEST, block[i - 4]->instruction.label, targetLabel, &block[i - 4]->assignment.expressions);
							}

							if (index != INVALID_ID) {
								expression = conditionBuilder.build_condition();
								if (!expression) break;
								block[i]->assignment.expressions.back() = expression;

								for (uint32_t j = index; j < i; j++) {
									switch (block[j]->type) {
									case AST_STATEMENT_CONDITION:
										if (block[j]->instruction.target == function.labels[targetLabel].target) (*block[i]->assignment.variables.back().slotScope)->usages--;
										function.remove_jump(block[j]->instruction.id + 1, block[j]->instruction.target);
										if (block[j]->assignment.variables.size()) function.remove_jump(block[j]->instruction.id, block[j]->instruction.id + 2);
										continue;
									case AST_STATEMENT_GOTO:
									case AST_STATEMENT_BREAK:
										function.remove_jump(block[j]->instruction.id, block[j]->instruction.target);
										continue;
									case AST_STATEMENT_ASSIGNMENT:
										(*block[i]->assignment.variables.back().slotScope)->usages--;
										continue;
									}
								}

								block[i]->instruction.label = block[index]->instruction.label;
								block[i]->assignment.isTableConstructor = false;
								block.erase(block.begin() + index, block.begin() + i);
								i = index;
							}
						}
					} else {
						if ((*block[i]->assignment.variables.back().slotScope)->usages == 1
							&& (i == block.size() - 1
								|| block[i + 1]->type != AST_STATEMENT_DECLARATION))
							break;
						//TODO
					}

					break;
				case AST_VARIABLE_TABLE_INDEX:
					if (i
						&& !function.is_valid_label(block[i]->instruction.label)
						&& block[i - 1]->type == AST_STATEMENT_ASSIGNMENT
						&& block[i - 1]->assignment.variables.size() == 1
						&& block[i - 1]->assignment.variables.back().type == AST_VARIABLE_SLOT
						&& block[i - 1]->assignment.variables.back().slot == block[i]->assignment.variables.back().table->variable->slot) {
						if (block[i - 1]->assignment.isTableConstructor
							&& !block[i - 1]->assignment.expressions.back()->table->multresField
							&& (block[i]->assignment.variables.back().isMultres
								|| get_constant_type(block[i]->assignment.variables.back().tableIndex) <= NIL_CONSTANT
								|| !get_constant_type(block[i]->assignment.expressions.back()))
							&& (block[i]->assignment.variables.back().isMultres
								|| !has_self_reference(block[i - 1]->assignment.variables.back().slot, block[i]->assignment.variables.back().tableIndex))
							&& !has_self_reference(block[i - 1]->assignment.variables.back().slot, block[i]->assignment.expressions.back())) {
							if (block[i]->assignment.variables.back().isMultres) {
								block[i - 1]->assignment.expressions.back()->table->multresIndex = block[i]->assignment.variables.back().multresIndex;
								block[i - 1]->assignment.expressions.back()->table->multresField = block[i]->assignment.expressions.back();
							} else {
								if (block[i]->assignment.variables.back().tableIndex->type == AST_EXPRESSION_CONSTANT && block[i]->assignment.variables.back().tableIndex->constant->type == AST_CONSTANT_STRING) {
									for (uint32_t j = block[i - 1]->assignment.expressions.back()->table->constants.fields.size(); j--;) {
										if (block[i - 1]->assignment.expressions.back()->table->constants.fields[j].key->constant->type != AST_CONSTANT_STRING
											|| block[i - 1]->assignment.expressions.back()->table->constants.fields[j].key->constant->string != block[i]->assignment.variables.back().tableIndex->constant->string)
											continue;
										if (block[i - 1]->assignment.expressions.back()->table->constants.fields[j].value->constant->type == AST_CONSTANT_NIL)
											block[i - 1]->assignment.expressions.back()->table->constants.fields.erase(block[i - 1]->assignment.expressions.back()->table->constants.fields.begin() + j);
										break;
									}
								}

								block[i - 1]->assignment.expressions.back()->table->fields.emplace_back();
								block[i - 1]->assignment.expressions.back()->table->fields.back().key = block[i]->assignment.variables.back().tableIndex;
								block[i - 1]->assignment.expressions.back()->table->fields.back().value = block[i]->assignment.expressions.back();
							}

							// 被合并语句的 table/tableIndex 读取从树中移除, 同步递减引用表;
							// value 表达式移入构造器, 计数不变。
							if (SlotScope* tableScope = *block[i]->assignment.variables.back().table->variable->slotScope) refCounts[tableScope]--;
							if (block[i]->assignment.variables.back().tableIndex
								&& block[i]->assignment.variables.back().tableIndex->type == AST_EXPRESSION_VARIABLE
								&& block[i]->assignment.variables.back().tableIndex->variable->type == AST_VARIABLE_SLOT) {
								if (SlotScope* indexScope = *block[i]->assignment.variables.back().tableIndex->variable->slotScope) refCounts[indexScope]--;
							}
							(*block[i - 1]->assignment.variables.back().slotScope)->usages--;
							block.erase(block.begin() + i);
							i -= 2;
							break;
						}

						if (!block[i]->assignment.variables.back().isMultres && (*block[i - 1]->assignment.variables.back().slotScope)->usages == 1) {
							if (SlotScope* tableScope = *block[i]->assignment.variables.back().table->variable->slotScope) refCounts[tableScope]--;
							block[i]->assignment.variables.back().table = block[i - 1]->assignment.expressions.back();
							function.slotScopeCollector.remove_scope(block[i - 1]->assignment.variables.back().slot, block[i - 1]->assignment.variables.back().slotScope);
							block[i]->instruction.label = block[i - 1]->instruction.label;
							i--;
							block.erase(block.begin() + i);
							break;
						}
					}

					if (block[i]->assignment.variables.back().isMultres) {
						// 某些字节码变体中多返回值调用直接写入表下标 (TSETM),
						// 无法合并进表构造器时保留为 `t[k] = f()` 形式。
					}
					break;
				}
			}

			break;
		}
	}
}

bool expression_references_scope(const Ast::Expression* expression, const Ast::SlotScope* scope) {
	if (!expression || !scope) return false;

	switch (expression->type) {
	case Ast::AST_EXPRESSION_VARIABLE:
		if (expression->variable->type == Ast::AST_VARIABLE_SLOT
			&& expression->variable->slotScope
			&& *expression->variable->slotScope == scope)
			return true;
		if (expression_references_scope(expression->variable->table, scope)) return true;
		return expression_references_scope(expression->variable->tableIndex, scope);
	case Ast::AST_EXPRESSION_FUNCTION_CALL:
		if (expression_references_scope(expression->functionCall->function, scope)) return true;
		for (const Ast::Expression* argument : expression->functionCall->arguments) {
			if (expression_references_scope(argument, scope)) return true;
		}
		return expression_references_scope(expression->functionCall->multresArgument, scope);
	case Ast::AST_EXPRESSION_TABLE:
		for (const auto& field : expression->table->fields) {
			if (expression_references_scope(field.key, scope) || expression_references_scope(field.value, scope)) return true;
		}
		return expression_references_scope(expression->table->multresField, scope);
	case Ast::AST_EXPRESSION_BINARY_OPERATION:
		return expression_references_scope(expression->binaryOperation->leftOperand, scope)
			|| expression_references_scope(expression->binaryOperation->rightOperand, scope);
	case Ast::AST_EXPRESSION_UNARY_OPERATION:
		return expression_references_scope(expression->unaryOperation->operand, scope);
	default:
		break;
	}

	return false;
}

bool expression_references_slot(const Ast::Expression* expression, const uint8_t slot) {
	if (!expression) return false;

	switch (expression->type) {
	case Ast::AST_EXPRESSION_VARIABLE:
		if (expression->variable->type == Ast::AST_VARIABLE_SLOT && expression->variable->slot == slot) return true;
		if (expression_references_slot(expression->variable->table, slot)) return true;
		return expression_references_slot(expression->variable->tableIndex, slot);
	case Ast::AST_EXPRESSION_FUNCTION_CALL:
		if (expression_references_slot(expression->functionCall->function, slot)) return true;
		for (const Ast::Expression* argument : expression->functionCall->arguments) {
			if (expression_references_slot(argument, slot)) return true;
		}
		return expression_references_slot(expression->functionCall->multresArgument, slot);
	case Ast::AST_EXPRESSION_TABLE:
		for (const auto& field : expression->table->fields) {
			if (expression_references_slot(field.key, slot) || expression_references_slot(field.value, slot)) return true;
		}
		return expression_references_slot(expression->table->multresField, slot);
	case Ast::AST_EXPRESSION_BINARY_OPERATION:
		return expression_references_slot(expression->binaryOperation->leftOperand, slot)
			|| expression_references_slot(expression->binaryOperation->rightOperand, slot);
	case Ast::AST_EXPRESSION_UNARY_OPERATION:
		return expression_references_slot(expression->unaryOperation->operand, slot);
	default:
		break;
	}

	return false;
}

bool expression_has_side_effects(const Ast::Expression* expression) {
	if (!expression) return false;

	switch (expression->type) {
	case Ast::AST_EXPRESSION_FUNCTION_CALL:
		return true;
	case Ast::AST_EXPRESSION_VARARG:
		return true;
	case Ast::AST_EXPRESSION_VARIABLE:
		if (expression_has_side_effects(expression->variable->table)) return true;
		return expression_has_side_effects(expression->variable->tableIndex);
	case Ast::AST_EXPRESSION_TABLE:
		if (expression->table->multresField) return true;
		for (const auto& field : expression->table->fields) {
			if (expression_has_side_effects(field.key) || expression_has_side_effects(field.value)) return true;
		}
		return false;
	case Ast::AST_EXPRESSION_BINARY_OPERATION:
		return expression_has_side_effects(expression->binaryOperation->leftOperand)
			|| expression_has_side_effects(expression->binaryOperation->rightOperand);
	case Ast::AST_EXPRESSION_UNARY_OPERATION:
		return expression_has_side_effects(expression->unaryOperation->operand);
	default:
		break;
	}

	return false;
}

uint32_t count_scope_reads_in_block(const std::vector<Ast::Statement*>& block, const Ast::SlotScope* scope) {
	uint32_t count = 0;

	const auto walkExpression = [&](const auto& self, const Ast::Expression* expression) -> void {
		if (!expression) return;
		switch (expression->type) {
		case Ast::AST_EXPRESSION_VARIABLE:
			if (expression->variable->type == Ast::AST_VARIABLE_SLOT
				&& expression->variable->slotScope
				&& *expression->variable->slotScope == scope)
				count++;
			if (expression->variable->table) self(self, expression->variable->table);
			if (expression->variable->tableIndex) self(self, expression->variable->tableIndex);
			break;
		case Ast::AST_EXPRESSION_FUNCTION_CALL:
			self(self, expression->functionCall->function);
			for (const Ast::Expression* argument : expression->functionCall->arguments) self(self, argument);
			if (expression->functionCall->multresArgument) self(self, expression->functionCall->multresArgument);
			break;
		case Ast::AST_EXPRESSION_TABLE:
			for (const auto& field : expression->table->fields) {
				self(self, field.key);
				self(self, field.value);
			}
			if (expression->table->multresField) self(self, expression->table->multresField);
			break;
		case Ast::AST_EXPRESSION_BINARY_OPERATION:
			self(self, expression->binaryOperation->leftOperand);
			self(self, expression->binaryOperation->rightOperand);
			break;
		case Ast::AST_EXPRESSION_UNARY_OPERATION:
			self(self, expression->unaryOperation->operand);
			break;
		default:
			break;
		}
	};

	const auto walkStatement = [&](const auto& self, const Ast::Statement* statement) -> void {
		if (!statement) return;
		for (const Ast::Expression* expression : statement->assignment.expressions) walkExpression(walkExpression, expression);
		for (const Ast::Variable& variable : statement->assignment.variables) {
			walkExpression(walkExpression, variable.table);
			walkExpression(walkExpression, variable.tableIndex);
		}
		if (statement->assignment.multresReturn) walkExpression(walkExpression, statement->assignment.multresReturn);
		for (const Ast::Statement* child : statement->block) self(self, child);
	};

	for (const Ast::Statement* statement : block) walkStatement(walkStatement, statement);
	return count;
}

void collect_scope_reads(const Ast::Function& function, std::unordered_map<const Ast::SlotScope*, uint32_t>& refCounts) {
	const auto walkExpression = [&](const auto& self, const Ast::Expression* expression) -> void {
		if (!expression) return;
		switch (expression->type) {
		case Ast::AST_EXPRESSION_VARIABLE:
			if (expression->variable->type == Ast::AST_VARIABLE_SLOT
				&& expression->variable->slotScope
				&& *expression->variable->slotScope)
				refCounts[*expression->variable->slotScope]++;
			if (expression->variable->table) self(self, expression->variable->table);
			if (expression->variable->tableIndex) self(self, expression->variable->tableIndex);
			break;
		case Ast::AST_EXPRESSION_FUNCTION_CALL:
			self(self, expression->functionCall->function);
			for (const Ast::Expression* argument : expression->functionCall->arguments) self(self, argument);
			if (expression->functionCall->multresArgument) self(self, expression->functionCall->multresArgument);
			break;
		case Ast::AST_EXPRESSION_TABLE:
			for (const auto& field : expression->table->fields) {
				self(self, field.key);
				self(self, field.value);
			}
			if (expression->table->multresField) self(self, expression->table->multresField);
			break;
		case Ast::AST_EXPRESSION_BINARY_OPERATION:
			self(self, expression->binaryOperation->leftOperand);
			self(self, expression->binaryOperation->rightOperand);
			break;
		case Ast::AST_EXPRESSION_UNARY_OPERATION:
			self(self, expression->unaryOperation->operand);
			break;
		default:
			break;
		}
	};

	const auto walkStatement = [&](const auto& self, const Ast::Statement* statement) -> void {
		if (!statement) return;
		for (const Ast::Expression* expression : statement->assignment.expressions) walkExpression(walkExpression, expression);
		for (const Ast::Variable& variable : statement->assignment.variables) {
			walkExpression(walkExpression, variable.table);
			walkExpression(walkExpression, variable.tableIndex);
		}
		if (statement->assignment.multresReturn) walkExpression(walkExpression, statement->assignment.multresReturn);
		for (const Ast::Statement* child : statement->block) self(self, child);
	};

	for (const Ast::Statement* statement : function.block) walkStatement(walkStatement, statement);
}

void collect_written_scopes(const Ast::Function& function, std::unordered_set<const Ast::SlotScope*>& writtenScopes) {
	const auto walkStatement = [&](const auto& self, const Ast::Statement* statement) -> void {
		if (!statement) return;
		for (const Ast::Variable& variable : statement->assignment.variables) {
			if (variable.type == Ast::AST_VARIABLE_SLOT && variable.slotScope && *variable.slotScope) {
				writtenScopes.insert(*variable.slotScope);
			}
		}
		for (const Ast::Statement* child : statement->block) self(self, child);
	};

	for (const Ast::Statement* statement : function.block) walkStatement(walkStatement, statement);
}

// 基于原始字节码指令构建 CFG, 用 Lengauer-Tarjan 计算支配树
// (O(n α(n)) 时间 / O(n) 内存, 大函数 (数万条指令) 也安全)。
// 返回 idom[指令id]: 该指令的直接支配者指令 id (入口支配自己)。
// 支配关系是跨分支/goto 传播的安全依据: 写入指令支配使用指令时,
// 从函数入口到使用点的所有路径都必然经过写入者。
std::vector<uint32_t> compute_cfg_idom(const Bytecode::Prototype& prototype) {
	const size_t n = prototype.instructions.size();
	if (!n) return std::vector<uint32_t>();

	std::vector<std::vector<uint32_t>> predecessors(n);
	std::vector<std::vector<uint32_t>> successors(n);
	const auto addEdge = [&](uint32_t from, uint32_t to) {
		if (to < n) {
			predecessors[to].push_back(from);
			successors[from].push_back(to);
		}
	};

	for (uint32_t i = 0; i < n; i++) {
		const Bytecode::Instruction& ins = prototype.instructions[i];
		bool jumps = false;
		bool fallsThrough = true;

		switch (ins.type) {
		// LuaJIT 的 IS* 条件指令后紧跟一条 JMP (group_jumps 合并时把 JMP 删除、
		// 目标抄给条件)。CFG 中: 条件分支跳转到下一条 JMP 的目标, 顺延落到 i+2。
		case Bytecode::BC_OP_ISLT:
		case Bytecode::BC_OP_ISGE:
		case Bytecode::BC_OP_ISLE:
		case Bytecode::BC_OP_ISGT:
		case Bytecode::BC_OP_ISEQV:
		case Bytecode::BC_OP_ISNEV:
		case Bytecode::BC_OP_ISEQS:
		case Bytecode::BC_OP_ISNES:
		case Bytecode::BC_OP_ISEQN:
		case Bytecode::BC_OP_ISNEN:
		case Bytecode::BC_OP_ISEQP:
		case Bytecode::BC_OP_ISNEP:
		case Bytecode::BC_OP_ISTC:
		case Bytecode::BC_OP_ISFC:
		case Bytecode::BC_OP_IST:
		case Bytecode::BC_OP_ISF:
			if (i + 1 < n) {
				const uint32_t jmpTarget = (i + 1) + (uint32_t)(prototype.instructions[i + 1].d - Bytecode::BC_OP_JMP_BIAS + 1);
				addEdge(i, i + 2);
				addEdge(i, jmpTarget);
			}
			continue;
		case Bytecode::BC_OP_FORI:
		case Bytecode::BC_OP_JFORI:
		case Bytecode::BC_OP_FORL:
		case Bytecode::BC_OP_IFORL:
		case Bytecode::BC_OP_JFORL:
		case Bytecode::BC_OP_ITERL:
		case Bytecode::BC_OP_IITERL:
		case Bytecode::BC_OP_JITERL:
		case Bytecode::BC_OP_LOOP:
		case Bytecode::BC_OP_ILOOP:
		case Bytecode::BC_OP_JLOOP:
			jumps = true;
			fallsThrough = true;
			break;
		case Bytecode::BC_OP_JMP:
		case Bytecode::BC_OP_UCLO:
		case Bytecode::BC_OP_ISNEXT:
			jumps = true;
			fallsThrough = false;
			break;
		case Bytecode::BC_OP_RETM:
		case Bytecode::BC_OP_RET:
		case Bytecode::BC_OP_RET0:
		case Bytecode::BC_OP_RET1:
		case Bytecode::BC_OP_CALLMT:
		case Bytecode::BC_OP_CALLT:
			fallsThrough = false;
			break;
		default:
			break;
		}

		const uint32_t target = i + (uint32_t)(ins.d - Bytecode::BC_OP_JMP_BIAS + 1);
		if (jumps) addEdge(i, target);
		if (fallsThrough) addEdge(i, i + 1);
	}

	// Lengauer-Tarjan 支配树 (节点按 DFS 编号)。
	std::vector<uint32_t> dfsNum(n, 0), vertex;         // 指令id -> DFS号; DFS号 -> 指令id
	std::vector<uint32_t> parent(n, 0), semi(n, 0), idom(n, 0);
	std::vector<uint32_t> ancestor(n, 0), label(n, 0);
	std::vector<std::vector<uint32_t>> bucket(n);

	uint32_t dfsCounter = 0;
	// 迭代 DFS: 超长直线函数 (数万条指令) 递归会爆栈。
	{
		std::vector<uint32_t> stack;
		stack.push_back(0);
		while (!stack.empty()) {
			const uint32_t v = stack.back();
			stack.pop_back();
			if (dfsNum[v]) continue;
			dfsNum[v] = dfsCounter;
			vertex.push_back(v);
			dfsCounter++;
			semi[v] = dfsNum[v];
			for (uint32_t w : successors[v]) {
				if (!dfsNum[w]) {
					parent[w] = v;
					stack.push_back(w);
				}
			}
		}
	}

	// 迭代 compress: 并查集链在极端情况下也可能很深。
	const auto compress = [&](uint32_t v) -> void {
		std::vector<uint32_t> path;
		uint32_t current = v;
		while (ancestor[ancestor[current]]) {
			path.push_back(current);
			current = ancestor[current];
		}
		for (size_t i = path.size(); i-- > 0;) {
			const uint32_t x = path[i];
			if (semi[label[ancestor[x]]] < semi[label[x]]) label[x] = label[ancestor[x]];
			ancestor[x] = ancestor[ancestor[x]];
		}
	};
	const auto eval = [&](uint32_t v) -> uint32_t {
		if (!ancestor[v]) return label[v];
		compress(v);
		return semi[label[ancestor[v]]] >= semi[label[v]] ? label[v] : label[ancestor[v]];
	};
	const auto link = [&](uint32_t v, uint32_t w) {
		ancestor[w] = v;
	};

	for (uint32_t i = 0; i < n; i++) label[i] = i;

	for (uint32_t i = dfsCounter; i-- > 1;) {
		const uint32_t w = vertex[i];
		for (uint32_t v : predecessors[w]) {
			if (!dfsNum[v]) continue;
			const uint32_t u = eval(v);
			if (semi[u] < semi[w]) semi[w] = semi[u];
		}
		bucket[vertex[semi[w]]].push_back(w);
		link(parent[w], w);
		for (uint32_t v : bucket[parent[w]]) {
			const uint32_t u = eval(v);
			idom[v] = semi[u] < semi[v] ? u : parent[w];
		}
		bucket[parent[w]].clear();
	}

	for (uint32_t i = 1; i < dfsCounter; i++) {
		const uint32_t w = vertex[i];
		if (idom[w] != vertex[semi[w]]) idom[w] = idom[idom[w]];
	}
	idom[vertex[0]] = vertex[0];

	// 转回指令 id 索引: idomByIns[ins] = 直接支配者指令 id。
	std::vector<uint32_t> idomByIns(n, 0);
	for (uint32_t i = 0; i < dfsCounter; i++) {
		idomByIns[vertex[i]] = vertex[i] == 0 ? vertex[0] : idom[vertex[i]];
	}
	return idomByIns;
}

// 收集被所有子函数 (闭包) 通过 upvalue 捕获的槽位作用域。
void collect_captured_scopes(const Ast::Function& function, std::unordered_set<const Ast::SlotScope*>& capturedScopes) {
	const auto collect = [&](const auto& self, const Ast::Function& fn) -> void {
		for (const Ast::Function* child : fn.childFunctions) {
			for (const Ast::Function::Upvalue& upvalue : child->upvalues) {
				if (upvalue.slotScope && *upvalue.slotScope) {
					capturedScopes.insert(*upvalue.slotScope);
				}
			}
			self(self, *child);
		}
	};
	collect(collect, function);
}

void Ast::propagate_cross_block_copies(Function& function) {
	// 跨块拷贝传播: 编译器为方法调用/表达式生成的临时槽可能横跨 goto/label
	// 重构出的多个块 (如 `local var = self.list` 在 if 分支, 使用在合并后的块),
	// 单趟链消除的连续窗口够不到。这里用原始字节码 CFG 支配 + 无中间改写做保守传播。
	// 固定点: 一轮传播内联可能引入新的跨块引用, 迭代直到不再有写入者被删除,
	// 避免 A 内联引入的 B 引用赶不上 B 的删除判定而留下幽灵槽。
	std::unordered_set<const SlotScope*> writtenScopes;
	collect_written_scopes(function, writtenScopes);

	// 支配树按需计算: 没有槽位引用候选时 (如纯常量大表函数) 跳过昂贵的 LT。
	std::vector<uint32_t> cfgIdom;
	bool cfgIdomReady = false;
	const auto cfgDominates = [&](uint32_t a, uint32_t b) -> bool {
		if (a >= cfgIdom.size() || b >= cfgIdom.size()) return false;
		if (a == b) return true;
		if (b != 0 && cfgIdom[b] == 0) return false; // b 不可达
		uint32_t current = b;
		while (current != 0) {
			current = cfgIdom[current];
			if (current == a) return true;
		}
		return a == 0;
	};

	// 子函数 (闭包) 通过 upvalue 捕获父函数槽位: 这些引用不在当前函数树里,
	// 删除写入者会让闭包内的引用变成无名幽灵槽, 因此捕获的作用域不删除写入者。
	std::unordered_set<const SlotScope*> capturedScopes;
	collect_captured_scopes(function, capturedScopes);

	const auto onePass = [&]() -> bool {
		bool erasedAny = false;

	// 1) 建立语句的父块/父语句/块内下标关系。
	std::unordered_map<Statement*, std::vector<Statement*>*> ownerBlock;
	std::unordered_map<Statement*, size_t> blockIndex;

	const auto buildParent = [&](const auto& self, std::vector<Statement*>& block) -> void {
		for (size_t j = 0; j < block.size(); j++) {
			Statement* statement = block[j];
			if (!statement) continue;
			ownerBlock[statement] = &block;
			blockIndex[statement] = j;
			if (!statement->block.empty()) self(self, statement->block);
		}
	};
	buildParent(buildParent, function.block);

	// 2) 收集单写入者作用域、多写入者集合、引用计数。
	std::unordered_map<SlotScope*, Statement*> writerStatement;
	std::unordered_set<SlotScope*> multiWriter;
	std::unordered_map<uint8_t, std::vector<Statement*>> allWritersBySlot;
	std::unordered_map<uint8_t, std::vector<Statement*>> pureWritersBySlot;

	const auto collectWriter = [&](const auto& self, Statement* statement) -> void {
		if (!statement) return;
		// 多变量语句 (多返回值 CALL 等) 的结果槽由 build_multi_assignment 合并,
		// 整体删除会连累其它结果槽, 一律不参与传播。
		if (statement->assignment.variables.size() != 1) {
			for (const Variable& variable : statement->assignment.variables) {
				if (variable.type == AST_VARIABLE_SLOT && variable.slotScope && *variable.slotScope) {
					multiWriter.insert(*variable.slotScope);
				}
			}
		}
		for (const Variable& variable : statement->assignment.variables) {
			if (variable.type != AST_VARIABLE_SLOT || !variable.slotScope || !*variable.slotScope) continue;
			allWritersBySlot[variable.slot].emplace_back(statement);
			SlotScope* scope = *variable.slotScope;
			auto it = writerStatement.find(scope);
			if (it == writerStatement.end()) {
				writerStatement[scope] = statement;
			} else if (it->second != statement) {
				multiWriter.insert(scope);
			}
		}
		// 纯 RHS 单槽写入者: 常量/全局/表索引等无副作用表达式, 即使作用域
		// 被其它写入复用 (槽位合并), 支配 + 无中间重写时内联仍安全。
		if (statement->instruction.label == INVALID_ID
			&& statement->assignment.variables.size() == 1
			&& statement->assignment.variables.back().type == AST_VARIABLE_SLOT
			&& statement->assignment.variables.back().slotScope
			&& *statement->assignment.variables.back().slotScope
			&& statement->assignment.expressions.size() == 1
			&& statement->assignment.expressions.back()
			&& !expression_has_side_effects(statement->assignment.expressions.back())) {
			pureWritersBySlot[statement->assignment.variables.back().slot].emplace_back(statement);
		}
		for (Statement* child : statement->block) self(self, child);
	};
	{
		const auto walkRoot = [&](const auto& self, std::vector<Statement*>& blk) -> void {
			for (Statement* statement : blk) {
				if (statement) {
					collectWriter(collectWriter, statement);
					if (!statement->block.empty()) self(self, statement->block);
				}
			}
		};
		walkRoot(walkRoot, function.block);
	}

	// 3) 收集所有槽位引用的树内位置 (Expression**) 及其所属语句。
	std::unordered_map<const SlotScope*, std::vector<std::pair<Expression**, Statement*>>> uses;
	std::unordered_map<uint8_t, std::vector<std::pair<Expression**, Statement*>>> usesBySlot;

	const auto collectUses = [&](const auto& self, Expression** slot, Expression* expression, Statement* owner) -> void {
		if (!expression) return;
		switch (expression->type) {
		case AST_EXPRESSION_VARIABLE:
			if (expression->variable->type == AST_VARIABLE_SLOT) {
				if (expression->variable->slotScope && *expression->variable->slotScope) {
					uses[*expression->variable->slotScope].emplace_back(slot, owner);
				}
				// 槽位兜底不要求作用域非空: 合并别名/幽灵作用域同样按槽位安全传播。
				usesBySlot[expression->variable->slot].emplace_back(slot, owner);
			}
			if (expression->variable->table) self(self, &expression->variable->table, expression->variable->table, owner);
			if (expression->variable->tableIndex) self(self, &expression->variable->tableIndex, expression->variable->tableIndex, owner);
			break;
		case AST_EXPRESSION_FUNCTION_CALL:
			if (expression->functionCall->function) self(self, &expression->functionCall->function, expression->functionCall->function, owner);
			for (size_t j = 0; j < expression->functionCall->arguments.size(); j++) {
				self(self, &expression->functionCall->arguments[j], expression->functionCall->arguments[j], owner);
			}
			if (expression->functionCall->multresArgument) {
				self(self, &expression->functionCall->multresArgument, expression->functionCall->multresArgument, owner);
			}
			break;
		case AST_EXPRESSION_TABLE:
			for (auto& field : expression->table->fields) {
				self(self, &field.key, field.key, owner);
				self(self, &field.value, field.value, owner);
			}
			if (expression->table->multresField) self(self, &expression->table->multresField, expression->table->multresField, owner);
			break;
		case AST_EXPRESSION_BINARY_OPERATION:
			self(self, &expression->binaryOperation->leftOperand, expression->binaryOperation->leftOperand, owner);
			self(self, &expression->binaryOperation->rightOperand, expression->binaryOperation->rightOperand, owner);
			break;
		case AST_EXPRESSION_UNARY_OPERATION:
			self(self, &expression->unaryOperation->operand, expression->unaryOperation->operand, owner);
			break;
		default:
			break;
		}
	};

	const auto walkStatementUses = [&](const auto& self, Statement* statement) -> void {
		if (!statement) return;
		for (size_t j = 0; j < statement->assignment.expressions.size(); j++) {
			collectUses(collectUses, &statement->assignment.expressions[j], statement->assignment.expressions[j], statement);
		}
		if (statement->assignment.multresReturn) {
			collectUses(collectUses, &statement->assignment.multresReturn, statement->assignment.multresReturn, statement);
		}
		if (statement->assignment.variables.size() == 1 && statement->assignment.variables.back().tableIndex) {
			collectUses(collectUses, &statement->assignment.variables.back().tableIndex,
				statement->assignment.variables.back().tableIndex, statement);
		}
		if (statement->assignment.variables.size() == 1 && statement->assignment.variables.back().table) {
			collectUses(collectUses, &statement->assignment.variables.back().table,
				statement->assignment.variables.back().table, statement);
		}
		for (Statement* child : statement->block) self(self, child);
	};
	{
		const auto walkRoot = [&](const auto& self, std::vector<Statement*>& blk) -> void {
			for (Statement* statement : blk) {
				if (statement) {
					walkStatementUses(walkStatementUses, statement);
					if (!statement->block.empty()) self(self, statement->block);
				}
			}
		};
		walkRoot(walkRoot, function.block);
	}

	// 支配树按需计算: 没有槽位引用候选时 (如纯常量大表函数) 跳过昂贵的 LT。
	if (!cfgIdomReady && !uses.empty()) {
		cfgIdom = compute_cfg_idom(function.prototype);
		cfgIdomReady = true;
	}

	// 4) 对每个单写入者作用域做支配检查并传播。
	const auto slotRefsInExpression = [&](const auto& self, std::unordered_set<uint8_t>& slots, const Expression* expression) -> void {
		if (!expression) return;
		switch (expression->type) {
		case AST_EXPRESSION_VARIABLE:
			if (expression->variable->type == AST_VARIABLE_SLOT) slots.insert(expression->variable->slot);
			if (expression->variable->table) self(self, slots, expression->variable->table);
			if (expression->variable->tableIndex) self(self, slots, expression->variable->tableIndex);
			break;
		case AST_EXPRESSION_FUNCTION_CALL:
			self(self, slots, expression->functionCall->function);
			for (const Expression* arg : expression->functionCall->arguments) self(self, slots, arg);
			if (expression->functionCall->multresArgument) self(self, slots, expression->functionCall->multresArgument);
			break;
		case AST_EXPRESSION_TABLE:
			for (const auto& field : expression->table->fields) {
				self(self, slots, field.key);
				self(self, slots, field.value);
			}
			if (expression->table->multresField) self(self, slots, expression->table->multresField);
			break;
		case AST_EXPRESSION_BINARY_OPERATION:
			self(self, slots, expression->binaryOperation->leftOperand);
			self(self, slots, expression->binaryOperation->rightOperand);
			break;
		case AST_EXPRESSION_UNARY_OPERATION:
			self(self, slots, expression->unaryOperation->operand);
			break;
		default:
			break;
		}
	};

	for (const auto& [scope, writer] : writerStatement) {
		if (multiWriter.contains(scope)) continue;
		if (!writer || writer->instruction.label != INVALID_ID) {
			continue;
		}
		if (writer->assignment.expressions.size() != 1 || !writer->assignment.expressions.back()) {
			continue;
		}

		Expression* rhs = writer->assignment.expressions.back();
		if (expression_references_scope(rhs, scope)) {
			continue;
		}

		// RHS 若是槽位引用, 要求其作用域最终会被命名 (参数或存在写入语句),
		// 否则内联会留下 `var_<槽位>` 兜底名。
		if (rhs->type == AST_EXPRESSION_VARIABLE && rhs->variable->type == AST_VARIABLE_SLOT) {
			SlotScope* rhsScope = *rhs->variable->slotScope;
			const uint8_t rhsSlot = rhs->variable->slot;
			const bool rhsIsParameter = rhsSlot < function.slotScopeCollector.slotInfos.size()
				&& function.slotScopeCollector.slotInfos[rhsSlot].isParameter;
			if (!rhsScope || (!rhsIsParameter && !writtenScopes.contains(rhsScope))) continue;
		}

		const bool impure = expression_has_side_effects(rhs);
		std::unordered_set<uint8_t> rhsSlots;
		slotRefsInExpression(slotRefsInExpression, rhsSlots, rhs);

		const uint8_t writerSlot = writer->assignment.variables.back().slot;
		auto useIt = uses.find(scope);
		bool slotFallback = false;
		std::vector<std::pair<Expression**, Statement*>>* useList = nullptr;
		if (useIt != uses.end()) {
			useList = &useIt->second;
		} else {
			// 合并别名场景: 读取引用的作用域与写入作用域不是同一对象
			// (如调试信息声明与后续赋值/读取的 SlotScope 分裂), 按槽位兜底。
			auto slotIt = usesBySlot.find(writerSlot);
			if (slotIt != usesBySlot.end()) {
				useList = &slotIt->second;
				slotFallback = true;
			}
		}
		if (!useList) continue;
		auto& scopeUses = *useList;

		// 带副作用 RHS 只有在作用域仅一个使用点 (传播后可删除写入语句) 时才安全,
		// 否则会把调用复制到多处。
		if (impure && scopeUses.size() != 1) {
			continue;
		}

		std::vector<Expression**> pendingReplacements;
		bool safe = true;

		for (const auto& [ref, useStatement] : scopeUses) {
			if (!useStatement) { safe = false; break; }
	
			// CFG 支配: 写入指令必须支配使用指令, 即从函数入口到使用点的
			// 所有路径都经过写入者。这正确处理了 if 分支 + goto 合并点:
			// 只有写入者所在路径才能到达使用点时, 传播是安全的。
			bool dominated = false;
			if (writer->instruction.id != INVALID_ID && useStatement->instruction.id != INVALID_ID) {
				dominated = cfgDominates(writer->instruction.id, useStatement->instruction.id);
			} else if (useStatement->instruction.id != INVALID_ID) {
				// 无指令 id 的声明 (调试信息 local): 用块内位置检查。
				std::vector<Statement*>* writerBlock = ownerBlock[writer];
				std::vector<Statement*>* useBlock = ownerBlock[useStatement];
				if (writerBlock && writerBlock == useBlock
					&& blockIndex[writer] < blockIndex[useStatement]) {
					dominated = true;
					for (size_t j = blockIndex[writer] + 1; j < blockIndex[useStatement]; j++) {
						if ((*writerBlock)[j] && function.is_valid_label((*writerBlock)[j]->instruction.label)) {
							dominated = false;
							break;
						}
					}
				}
			}
			if (!dominated) {
						safe = false;
				break;
			}

			// 槽位兜底时, 写入者与使用点之间不得有同槽位重写 (其它作用域也一样)。
			if (slotFallback && writer->instruction.id != INVALID_ID && useStatement->instruction.id != INVALID_ID) {
				const uint32_t begin = writer->instruction.id;
				const uint32_t end = useStatement->instruction.id;
				for (const auto& [otherScope, otherWriter] : writerStatement) {
					if (!otherWriter || otherWriter == writer
						|| otherWriter->instruction.id <= begin || otherWriter->instruction.id >= end) continue;
					for (const Variable& variable : otherWriter->assignment.variables) {
						if (variable.type == AST_VARIABLE_SLOT && variable.slot == writerSlot) {
							safe = false;
							break;
						}
					}
					if (!safe) break;
				}
			}
			if (!safe) break;

			// 无中间改写: 写入者与使用点之间, RHS 引用的任何槽位不得被重新赋值。
			if (!rhsSlots.empty() && writer->instruction.id != INVALID_ID && useStatement->instruction.id != INVALID_ID) {
				const uint32_t begin = writer->instruction.id;
				const uint32_t end = useStatement->instruction.id;
				for (const auto& [otherScope, otherWriter] : writerStatement) {
					if (!otherWriter || otherWriter->instruction.id <= begin || otherWriter->instruction.id >= end) continue;
					for (const Variable& variable : otherWriter->assignment.variables) {
						if (variable.type == AST_VARIABLE_SLOT && rhsSlots.contains(variable.slot)) {
							safe = false;
							break;
						}
					}
					if (!safe) break;
				}
			}
			if (!safe) break;

			pendingReplacements.emplace_back(ref);
		}

		if (!safe || pendingReplacements.empty()) continue;

		// 传播: 替换所有使用点引用; 无剩余引用时删除写入语句。
		for (Expression** ref : pendingReplacements) {
			if (!*ref || (*ref)->type != AST_EXPRESSION_VARIABLE
				|| (*ref)->variable->type != AST_VARIABLE_SLOT
				|| !(*ref)->variable->slotScope) {
				continue;
			}
			if (expression_references_scope(rhs, *(*ref)->variable->slotScope)) {
				safe = false;
				break;
			}
			*ref = rhs;
		}
		if (!safe) continue;

		bool eraseWriter = false;
		{
			const auto countRefs = [&](const auto& self, const Statement* statement) -> uint32_t {
				uint32_t count = 0;
				const auto countExpr = [&](const auto& self2, const Expression* expression) -> void {
					if (!expression) return;
					switch (expression->type) {
					case AST_EXPRESSION_VARIABLE:
						if (expression->variable->type == AST_VARIABLE_SLOT
							&& (slotFallback
								? expression->variable->slot == writerSlot
								: (expression->variable->slotScope
									&& *expression->variable->slotScope == scope)))
							count++;
						if (expression->variable->table) self2(self2, expression->variable->table);
						if (expression->variable->tableIndex) self2(self2, expression->variable->tableIndex);
						break;
					case AST_EXPRESSION_FUNCTION_CALL:
						self2(self2, expression->functionCall->function);
						for (const Expression* arg : expression->functionCall->arguments) self2(self2, arg);
						if (expression->functionCall->multresArgument) self2(self2, expression->functionCall->multresArgument);
						break;
					case AST_EXPRESSION_TABLE:
						for (const auto& field : expression->table->fields) {
							self2(self2, field.key);
							self2(self2, field.value);
						}
						if (expression->table->multresField) self2(self2, expression->table->multresField);
						break;
					case AST_EXPRESSION_BINARY_OPERATION:
						self2(self2, expression->binaryOperation->leftOperand);
						self2(self2, expression->binaryOperation->rightOperand);
						break;
					case AST_EXPRESSION_UNARY_OPERATION:
						self2(self2, expression->unaryOperation->operand);
						break;
					default: break;
					}
				};
				for (const Expression* expression : statement->assignment.expressions) countExpr(countExpr, expression);
				if (statement->assignment.multresReturn) countExpr(countExpr, statement->assignment.multresReturn);
				for (const Variable& variable : statement->assignment.variables) {
					if (variable.table) countExpr(countExpr, variable.table);
					if (variable.tableIndex) countExpr(countExpr, variable.tableIndex);
				}
				for (const Statement* child : statement->block) count += self(self, child);
				return count;
			};
			uint32_t total = 0;
			const auto walkRoot = [&](std::vector<Statement*>& block) -> void {
				for (Statement* statement : block) {
					if (statement) total += countRefs(countRefs, statement);
				}
			};
			walkRoot(function.block);
			eraseWriter = (total == 0);
		}

		if (eraseWriter && !capturedScopes.contains(scope)) {
			erasedAny = true;
			function.slotScopeCollector.remove_scope(writer->assignment.variables.back().slot, writer->assignment.variables.back().slotScope);
			auto blockIt = ownerBlock[writer];
			if (blockIt) {
				size_t erasedIndex = blockIndex[writer];
				auto& blk = *blockIt;
				for (size_t j = 0; j < blk.size(); j++) {
					if (blk[j] == writer) {
						erasedIndex = j;
						blk.erase(blk.begin() + j);
						break;
					}
				}
				// 同一块内后续语句的下标已偏移, 增量修正, 避免陈旧索引删错语句。
				for (auto& [statement, index] : blockIndex) {
					if (ownerBlock[statement] == blockIt && index > erasedIndex) index--;
				}
			}
		}
	}

	// 5) 纯 RHS 槽位传播: 常量/全局等无副作用写入者, 即使作用域被多写入
	//    (槽位复用/与真局部合并, 如 `local var_6_0 = 2` 与 value 共享槽),
	//    只要写入者支配使用点且中间无同槽重写, 内联依然安全。
	for (const auto& [slot, writers] : pureWritersBySlot) {
		auto slotUseIt = usesBySlot.find(slot);
		if (slotUseIt == usesBySlot.end()) continue;
		auto& slotUses = slotUseIt->second;

		for (Statement* writer : writers) {
			// 单写入者作用域已由上一轮处理。
			SlotScope* writerScope = *writer->assignment.variables.back().slotScope;
			auto scopeIt = writerStatement.find(writerScope);
			if (scopeIt != writerStatement.end()
				&& scopeIt->second == writer
				&& !multiWriter.contains(writerScope))
				continue;

			Expression* rhs = writer->assignment.expressions.back();
			std::unordered_set<uint8_t> rhsSlots;
			slotRefsInExpression(slotRefsInExpression, rhsSlots, rhs);

			std::vector<Expression**> pendingReplacements;
			bool safe = true;

			for (const auto& [ref, useStatement] : slotUses) {
				if (!useStatement) { safe = false; break; }

				// 支配: CFG (真实指令 id) 或块内位置 (无 id 声明)。
				bool dominated = false;
				if (writer->instruction.id != INVALID_ID && useStatement->instruction.id != INVALID_ID) {
					dominated = cfgDominates(writer->instruction.id, useStatement->instruction.id);
				} else if (useStatement->instruction.id != INVALID_ID) {
					std::vector<Statement*>* writerBlock = ownerBlock[writer];
					std::vector<Statement*>* useBlock = ownerBlock[useStatement];
					if (writerBlock && writerBlock == useBlock
						&& blockIndex[writer] < blockIndex[useStatement]) {
						dominated = true;
						for (size_t j = blockIndex[writer] + 1; j < blockIndex[useStatement]; j++) {
							if ((*writerBlock)[j] && function.is_valid_label((*writerBlock)[j]->instruction.label)) {
								dominated = false;
								break;
							}
						}
					}
				}
				if (!dominated) { safe = false; break; }

				// 写入者与使用点之间不得有同槽位重写。
				if (writer->instruction.id != INVALID_ID && useStatement->instruction.id != INVALID_ID) {
					const uint32_t begin = writer->instruction.id;
					const uint32_t end = useStatement->instruction.id;
					for (Statement* other : allWritersBySlot[slot]) {
						if (!other || other == writer
							|| other->instruction.id == INVALID_ID
							|| other->instruction.id <= begin || other->instruction.id >= end) continue;
						safe = false;
						break;
					}
				} else {
					std::vector<Statement*>* writerBlock = ownerBlock[writer];
					std::vector<Statement*>* useBlock = ownerBlock[useStatement];
					if (writerBlock && writerBlock == useBlock) {
						for (size_t j = blockIndex[writer] + 1; j < blockIndex[useStatement]; j++) {
							Statement* between = (*writerBlock)[j];
							if (!between) continue;
							for (const Variable& variable : between->assignment.variables) {
								if (variable.type == AST_VARIABLE_SLOT && variable.slot == slot) {
									safe = false;
									break;
								}
							}
							if (!safe) break;
						}
					}
				}
				if (!safe) break;

				// RHS 引用的槽位不得在写入者与使用点之间被重写。
				if (!rhsSlots.empty() && writer->instruction.id != INVALID_ID && useStatement->instruction.id != INVALID_ID) {
					const uint32_t begin = writer->instruction.id;
					const uint32_t end = useStatement->instruction.id;
					for (const auto& [otherScope, otherWriter] : writerStatement) {
						if (!otherWriter || otherWriter == writer
							|| otherWriter->instruction.id <= begin || otherWriter->instruction.id >= end) continue;
						for (const Variable& variable : otherWriter->assignment.variables) {
							if (variable.type == AST_VARIABLE_SLOT && rhsSlots.contains(variable.slot)) {
								safe = false;
								break;
							}
						}
						if (!safe) break;
					}
				}
				if (!safe) break;

				pendingReplacements.emplace_back(ref);
			}

			if (!safe || pendingReplacements.empty()) continue;

			for (Expression** ref : pendingReplacements) {
				if (!*ref || (*ref)->type != AST_EXPRESSION_VARIABLE
					|| (*ref)->variable->type != AST_VARIABLE_SLOT) {
					continue;
				}
				if ((*ref)->variable->slotScope
					&& *(*ref)->variable->slotScope
					&& expression_references_scope(rhs, *(*ref)->variable->slotScope)) {
					safe = false;
					break;
				}
				*ref = rhs;
			}
			if (!safe) continue;

			// 按槽位统计剩余引用, 无引用且未被闭包捕获时删除写入者。
			uint32_t remaining = 0;
			const auto countSlot = [&](const auto& self2, const Expression* expression) -> void {
				if (!expression) return;
				switch (expression->type) {
				case AST_EXPRESSION_VARIABLE:
					if (expression->variable->type == AST_VARIABLE_SLOT && expression->variable->slot == slot) remaining++;
					if (expression->variable->table) self2(self2, expression->variable->table);
					if (expression->variable->tableIndex) self2(self2, expression->variable->tableIndex);
					break;
				case AST_EXPRESSION_FUNCTION_CALL:
					self2(self2, expression->functionCall->function);
					for (const Expression* arg : expression->functionCall->arguments) self2(self2, arg);
					if (expression->functionCall->multresArgument) self2(self2, expression->functionCall->multresArgument);
					break;
				case AST_EXPRESSION_TABLE:
					for (const auto& field : expression->table->fields) {
						self2(self2, field.key);
						self2(self2, field.value);
					}
					if (expression->table->multresField) self2(self2, expression->table->multresField);
					break;
				case AST_EXPRESSION_BINARY_OPERATION:
					self2(self2, expression->binaryOperation->leftOperand);
					self2(self2, expression->binaryOperation->rightOperand);
					break;
				case AST_EXPRESSION_UNARY_OPERATION:
					self2(self2, expression->unaryOperation->operand);
					break;
				default: break;
				}
			};
			const auto countStmt = [&](const auto& self2, const Statement* statement) -> void {
				if (!statement) return;
				for (const Expression* expression : statement->assignment.expressions) countSlot(countSlot, expression);
				if (statement->assignment.multresReturn) countSlot(countSlot, statement->assignment.multresReturn);
				for (const Variable& variable : statement->assignment.variables) {
					countSlot(countSlot, variable.table);
					countSlot(countSlot, variable.tableIndex);
				}
				for (const Statement* child : statement->block) self2(self2, child);
			};
			const auto walkRoot = [&](std::vector<Statement*>& block) -> void {
				for (Statement* statement : block) {
					if (statement) countStmt(countStmt, statement);
				}
			};
			walkRoot(function.block);

			if (remaining == 0 && !capturedScopes.contains(writerScope)) {
				erasedAny = true;
				function.slotScopeCollector.remove_scope(writer->assignment.variables.back().slot, writer->assignment.variables.back().slotScope);
				auto blockIt = ownerBlock[writer];
				if (blockIt) {
					auto& blk = *blockIt;
					size_t erasedIndex = 0;
					for (size_t j = 0; j < blk.size(); j++) {
						if (blk[j] == writer) {
							erasedIndex = j;
							blk.erase(blk.begin() + j);
							break;
						}
					}
					for (auto& [statement, index] : blockIndex) {
						if (ownerBlock[statement] == blockIt && index > erasedIndex) index--;
					}
				}
			}
		}
	}

	return erasedAny;
	};

	while (onePass()) {}
}

void Ast::restore_method_calls(Function& function, std::vector<Statement*>& block) {
	for (uint32_t i = 0; i < block.size(); i++) {
		Statement* statement = block[i];
		if (!statement) continue;
		if (!statement->block.empty()) restore_method_calls(function, statement->block);

		FunctionCall* functionCall = nullptr;
		if ((statement->type == AST_STATEMENT_FUNCTION_CALL
				|| statement->type == AST_STATEMENT_ASSIGNMENT
				|| statement->type == AST_STATEMENT_DECLARATION)
			&& statement->assignment.expressions.size() == 1
			&& statement->assignment.expressions.back()
			&& statement->assignment.expressions.back()->type == AST_EXPRESSION_FUNCTION_CALL) {
			functionCall = statement->assignment.expressions.back()->functionCall;
		} else if (statement->type == AST_STATEMENT_RETURN
			&& statement->assignment.multresReturn
			&& statement->assignment.multresReturn->type == AST_EXPRESSION_FUNCTION_CALL) {
			functionCall = statement->assignment.multresReturn->functionCall;
		}

		if (!functionCall || functionCall->isMethod || functionCall->arguments.empty()) continue;

		// 模式: 函数位置是 `接收者.字段` (TGETS), 第一个参数就是接收者本身
		// (方法调用的 self 副本), 还原为 `接收者:字段(...)`。
		Expression* fn = functionCall->function;
		if (!fn || fn->type != AST_EXPRESSION_VARIABLE || fn->variable->type != AST_VARIABLE_TABLE_INDEX) continue;
		if (!fn->variable->tableIndex || fn->variable->tableIndex->type != AST_EXPRESSION_CONSTANT
			|| !fn->variable->tableIndex->constant->isName)
			continue;

		Expression* receiver = fn->variable->table;
		if (!receiver) continue;
		const bool receiverIsVariable = receiver->type == AST_EXPRESSION_VARIABLE
			&& (receiver->variable->type == AST_VARIABLE_SLOT || receiver->variable->type == AST_VARIABLE_UPVALUE);
		const bool receiverIsTableIndex = receiver->type == AST_EXPRESSION_VARIABLE
			&& receiver->variable->type == AST_VARIABLE_TABLE_INDEX;
		if (!receiverIsVariable && !receiverIsTableIndex)
			continue;
		if (!expressions_equal(*functionCall->arguments.front(), *receiver)) continue;

		bool ambiguous = false;
		for (size_t j = 1; j < functionCall->arguments.size(); j++) {
			if (expressions_equal(*functionCall->arguments[j], *receiver)) {
				ambiguous = true;
				break;
			}
		}
		if (ambiguous) continue;

		functionCall->isMethod = true;
		functionCall->arguments.erase(functionCall->arguments.begin());

		// 接收者若来自紧邻的单用途槽赋值 (UGET/GGET/TGETS 结果), 进一步内联,
		// 消除 `local var = self.list; self.list:push(...)` 中的接收者临时变量。
		if (!receiverIsVariable || receiver->variable->type != AST_VARIABLE_SLOT) continue;

		SlotScope* receiverScope = *receiver->variable->slotScope;
		if (!receiverScope) continue;
		uint32_t writer = INVALID_ID;

		for (uint32_t k = i, scanned = 0; k-- > 0 && scanned < 64;) {
			scanned++;
			Statement* previous = block[k];
			if (previous->type == AST_STATEMENT_GOTO || previous->type == AST_STATEMENT_EMPTY) {
				if (function.is_valid_label(previous->instruction.label)) break;
				continue;
			}
			if ((previous->type == AST_STATEMENT_ASSIGNMENT
					|| (previous->type == AST_STATEMENT_DECLARATION
						&& (*previous->assignment.variables.back().slotScope)->isSynthetic))
				&& previous->assignment.variables.size() == 1
				&& previous->assignment.expressions.size() == 1
				&& previous->assignment.variables.back().type == AST_VARIABLE_SLOT
				&& previous->assignment.variables.back().slot == receiver->variable->slot
				&& !function.is_valid_label(previous->instruction.label)
				&& count_scope_reads_in_block(block, receiverScope) == 1) {
				writer = k;
				break;
			}
			break;
		}

		if (writer == INVALID_ID) continue;

		Statement* writerStatement = block[writer];
		Expression* rhs = writerStatement->assignment.expressions.back();
		if (expression_references_scope(rhs, receiverScope)) continue;

		fn->variable->table = rhs;
		function.slotScopeCollector.remove_scope(writerStatement->assignment.variables.back().slot, writerStatement->assignment.variables.back().slotScope);
		block.erase(block.begin() + writer);
		i--;
	}
}
