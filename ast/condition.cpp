#include "../main.h"

// 各 pass 之间共享的内部辅助函数 (声明; 定义在 ast/eliminate.cpp)。
// 注意: 本头文件不包含 main.h, 引用前必须已包含之。


#include "ast_internal.h"

void Ast::eliminate_conditions(Function& function, std::vector<Statement*>& block, BlockInfo* const& previousBlock) {
	BlockInfo blockInfo = { .block = block, .previousBlock = previousBlock };
	std::vector<Expression*> expressions(1);
	uint32_t index, targetIndex, previousValidIndex, assignmentIndex, targetLabel, extendedTargetLabel;
	bool hasBoolConstruct, hasEndAssignment;

	for (uint32_t i = block.size(); i--;) {
		if (block[i]->instruction.id == INVALID_ID) continue;
		blockInfo.index = i;
		targetLabel = get_label_from_next_statement(function, blockInfo, false, false);
		extendedTargetLabel = get_label_from_next_statement(function, blockInfo, true, false);
		if (!function.is_valid_label(targetLabel) || function.labels[targetLabel].jumpIds.front() > block[i]->instruction.id) continue;

		switch (block[i]->type) {
		case AST_STATEMENT_CONDITION:
			index = INVALID_ID;

			for (uint32_t j = function.labels[targetLabel].jumpIds.size(); j--;) {
				if (function.labels[targetLabel].jumpIds[j] >= function.labels[targetLabel].target) continue;
				index = get_block_index_from_id(block, function.labels[targetLabel].jumpIds[j] - 1);
				if (index == INVALID_ID) break;

				switch (block[index]->type) {
				case AST_STATEMENT_CONDITION:
					if (!block[index]->assignment.variables.size()) {
						if (targetLabel == extendedTargetLabel
							|| (block[index]->assignment.expressions.size() == 1
								&& block[index]->assignment.expressions.back()->type == AST_EXPRESSION_VARIABLE
								&& block[index]->assignment.expressions.back()->variable->type == AST_VARIABLE_SLOT)) {
							index = INVALID_ID;
							continue;
						}

						index = INVALID_ID;
					}
						
					break;
				case AST_STATEMENT_ASSIGNMENT:
					if ((block[index + 1]->type == AST_STATEMENT_GOTO
							|| block[index + 1]->type == AST_STATEMENT_BREAK)
						&& block[index + 1]->instruction.type == Bytecode::BC_OP_JMP
						&& block[index]->assignment.variables.size() == 1
						&& block[index]->assignment.variables.back().type == AST_VARIABLE_SLOT
						&& block[index]->assignment.expressions.size() == 1)
						break;
				default:
					index = INVALID_ID;
				}

				break;
			}

			if (index == INVALID_ID) continue;
			assignmentIndex = index;
			break;
		case AST_STATEMENT_GOTO:
		case AST_STATEMENT_BREAK:
			if (!i
				|| function.is_valid_label(block[i]->instruction.label)
				|| block[i]->instruction.type != Bytecode::BC_OP_JMP
				|| block[i]->instruction.target != function.labels[targetLabel].target
				|| block[i - 1]->type != AST_STATEMENT_ASSIGNMENT
				|| block[i - 1]->assignment.variables.size() != 1
				|| block[i - 1]->assignment.variables.back().type != AST_VARIABLE_SLOT
				|| block[i - 1]->assignment.expressions.size() != 1)
				continue;
			assignmentIndex = i - 1;
			break;
		case AST_STATEMENT_ASSIGNMENT:
			if (block[i]->assignment.variables.size() != 1 || block[i]->assignment.variables.back().type != AST_VARIABLE_SLOT) continue;
			assignmentIndex = i;
			break;
		default:
			continue;
		}

		index = assignmentIndex;
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
			&& block[i - 2]->assignment.variables.back().slot == block[assignmentIndex]->assignment.variables.back().slot) {
			switch (block[i - 3]->type) {
			case AST_STATEMENT_CONDITION:
				if (block[i - 3]->assignment.expressions.size() == 2 && block[i - 3]->instruction.target == block[i]->instruction.id) hasBoolConstruct = true;
				break;
			case AST_STATEMENT_GOTO:
			case AST_STATEMENT_BREAK:
				if (i >= 4
					&& (!function.is_valid_label(block[i - 3]->instruction.label)
						|| (function.labels[block[i - 3]->instruction.label].jumpIds.size() == 1
							&& block[i - 4]->type == AST_STATEMENT_CONDITION
							&& block[i - 4]->assignment.variables.size()))
					&& block[i - 3]->instruction.type == Bytecode::BC_OP_JMP
					&& block[i - 3]->instruction.target == function.labels[extendedTargetLabel].target
					&& (function.is_valid_label(block[i]->instruction.label)
						|| function.is_valid_label(block[i - 2]->instruction.label)))
					hasBoolConstruct = true;
				break;
			}

			if (hasBoolConstruct) {
				if ((function.is_valid_label(block[i]->instruction.label)
					&& function.labels[block[i]->instruction.label].jumpIds.back() >= block[i]->instruction.id)
					|| (function.is_valid_label(block[i - 2]->instruction.label)
						&& function.labels[block[i - 2]->instruction.label].jumpIds.back() >= block[i - 2]->instruction.id))
					continue;

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

				if (index == INVALID_ID) continue;
			}
		}

		previousValidIndex = INVALID_ID;
		hasEndAssignment = hasBoolConstruct ? block[i - 3]->type == AST_STATEMENT_CONDITION || block[i - 4]->type == AST_STATEMENT_ASSIGNMENT : block[i]->type == AST_STATEMENT_ASSIGNMENT;
		targetIndex = hasBoolConstruct ? (block[i - 3]->type == AST_STATEMENT_GOTO ? i - (hasEndAssignment ? 4 : 3) : i - 2) : (hasEndAssignment ? i : i + 1);

		for (uint32_t j = function.labels[targetLabel].jumpIds.size(); j--;) {
			if (function.labels[targetLabel].jumpIds[j] > block[i]->instruction.id) continue;

			if (function.labels[targetLabel].jumpIds[j] < block[index]->instruction.id) {
				index = get_block_index_from_id(block, function.labels[targetLabel].jumpIds[j] - 1);

				if (hasBoolConstruct
					&& index == i - 2
					&& !function.is_valid_label(block[i]->instruction.label)) {
					index = get_block_index_from_id(block, function.labels[block[i - 2]->instruction.label].jumpIds.front() - 1);
					if (index == INVALID_ID) index = i - 2;
				}
			}

			for (uint32_t k = i; index != INVALID_ID && block[index]->instruction.id < block[k]->instruction.id; k--) {
				if (function.is_valid_label(block[k]->instruction.label)) {
					if (function.labels[block[k]->instruction.label].jumpIds.back() >= block[k]->instruction.id) {
						index = INVALID_ID;
						break;
					}

					while (function.labels[block[k]->instruction.label].jumpIds.front() < block[index]->instruction.id) {
						if (!index) {
							index = INVALID_ID;
							break;
						}

						index--;
					}
				}
			}

			if (index == INVALID_ID) break;

			switch (block[index]->type) {
			case AST_STATEMENT_GOTO:
			case AST_STATEMENT_BREAK:
				if (block[index]->instruction.target == function.labels[targetLabel].target && index) index--;
			}

			for (uint32_t k = index; k < targetIndex; k++) {
				switch (block[k]->type) {
				case AST_STATEMENT_CONDITION:
					if (block[k]->assignment.variables.size()) {
						if (block[k]->instruction.target == function.labels[targetLabel].target
							&& block[k]->assignment.variables.back().slot == block[assignmentIndex]->assignment.variables.back().slot)
							continue;
					} else if (block[k]->instruction.target == function.labels[targetLabel].target
						&& block[k]->assignment.expressions.size() == 1
						&& block[k]->assignment.expressions.back()->type == AST_EXPRESSION_VARIABLE
						&& block[k]->assignment.expressions.back()->variable->type == AST_VARIABLE_SLOT
						&& block[k]->assignment.expressions.back()->variable->slot == block[assignmentIndex]->assignment.variables.back().slot) {
						continue;
					} else if ((block[k]->instruction.target == function.labels[extendedTargetLabel].target
							&& !hasEndAssignment)
						|| (block[k]->instruction.target > block[k]->instruction.id
							&& block[k]->instruction.target < function.labels[targetLabel].target)) {
						continue;
					}

					break;
				case AST_STATEMENT_ASSIGNMENT:
					if (block[k]->assignment.variables.size() == 1
						&& block[k]->assignment.variables.back().type == AST_VARIABLE_SLOT
						&& block[k]->assignment.variables.back().slot == block[assignmentIndex]->assignment.variables.back().slot
						&& block[k]->assignment.expressions.size() == 1
						&& ++k != targetIndex
						&& (block[k]->type == AST_STATEMENT_GOTO
							|| block[k]->type == AST_STATEMENT_BREAK)
						&& !function.is_valid_label(block[k]->instruction.label)
						&& block[k]->instruction.type == Bytecode::BC_OP_JMP
						&& block[k]->instruction.target == function.labels[targetLabel].target)
						continue;
					break;
				}

				index = INVALID_ID;
				break;
			}

			if (index == INVALID_ID) break;
			previousValidIndex = index;
		}

		if (previousValidIndex == INVALID_ID) continue;
		index = previousValidIndex;

		ConditionBuilder conditionBuilder(ConditionBuilder::ASSIGNMENT, *this, targetLabel,
			hasBoolConstruct ? block[i]->instruction.label : INVALID_ID, hasBoolConstruct ? block[i - 2]->instruction.label : INVALID_ID);

		for (uint32_t j = index; j < targetIndex; j++) {
			switch (block[j]->type) {
			case AST_STATEMENT_CONDITION:
				conditionBuilder.add_node(conditionBuilder.get_node_type(block[j]->instruction.type, block[j]->condition.swapped), block[j]->instruction.label,
					hasEndAssignment
					|| block[j]->assignment.variables.size()
					|| (block[j]->instruction.target == function.labels[targetLabel].target
						? targetLabel != extendedTargetLabel
						|| (block[j]->assignment.expressions.size() == 1
							&& block[j]->assignment.expressions.back()->type == AST_EXPRESSION_VARIABLE
							&& block[j]->assignment.expressions.back()->variable->type == AST_VARIABLE_SLOT
							&& block[j]->assignment.expressions.back()->variable->slot == block[assignmentIndex]->assignment.variables.back().slot)
						: block[j]->instruction.target != function.labels[extendedTargetLabel].target)
					? function.get_label_from_id(block[j]->instruction.target) : function.labels.size(), &block[j]->assignment.expressions,
					block[j]->instruction.target == function.labels[targetLabel].target
					&& !hasEndAssignment
					&& block[j]->assignment.expressions.size() == 1
					&& block[j]->assignment.expressions.back()->type == AST_EXPRESSION_VARIABLE
					&& block[j]->assignment.expressions.back()->variable->type == AST_VARIABLE_SLOT
					&& block[j]->assignment.expressions.back()->variable->slot == block[assignmentIndex]->assignment.variables.back().slot
					&& targetLabel == extendedTargetLabel);
				continue;
			case AST_STATEMENT_ASSIGNMENT:
				if (block[j]->assignment.expressions.back()->type == AST_EXPRESSION_CONSTANT
					&& (block[j]->assignment.expressions.back()->constant->type == AST_CONSTANT_NIL
						|| block[j]->assignment.expressions.back()->constant->type == AST_CONSTANT_FALSE)) {
					conditionBuilder.add_node(ConditionBuilder::Node::FALSY_TEST, block[j]->instruction.label,
						function.get_label_from_id(block[j + 1]->instruction.target), &block[j]->assignment.expressions);
				} else {
					conditionBuilder.add_node(ConditionBuilder::Node::TRUTHY_TEST, block[j]->instruction.label,
						function.get_label_from_id(block[j + 1]->instruction.target), &block[j]->assignment.expressions);
				}

				j++;
				continue;
			}
		}

		if (hasEndAssignment) {
			if (!hasBoolConstruct) {
				conditionBuilder.add_node(ConditionBuilder::Node::TRUTHY_TEST, block[i]->instruction.label, targetLabel, &block[i]->assignment.expressions);
			} else if (block[i - 3]->type == AST_STATEMENT_GOTO) {
				conditionBuilder.add_node(ConditionBuilder::Node::TRUTHY_TEST, block[i - 4]->instruction.label, targetLabel, &block[i - 4]->assignment.expressions);
			}
		} else {
			expressions.back() = new_slot(block[assignmentIndex]->assignment.variables.back().slot);
			expressions.back()->variable->slotScope = block[assignmentIndex]->assignment.variables.back().slotScope;
			conditionBuilder.add_node(ConditionBuilder::Node::TRUTHY_TEST, function.labels.size(), targetLabel, &expressions);
		}
		
		expressions.back() = conditionBuilder.build_condition();
		if (!expressions.back()) continue;
		block[assignmentIndex]->assignment.expressions.back() = expressions.back();

		for (uint32_t j = index; j <= i; j++) {
			switch (block[j]->type) {
			case AST_STATEMENT_CONDITION:
				function.remove_jump(block[j]->instruction.id + 1, block[j]->instruction.target);
				if (!block[j]->assignment.variables.size()) continue;
				function.remove_jump(block[j]->instruction.id, block[j]->instruction.id + 2);
			case AST_STATEMENT_ASSIGNMENT:
				if (!block[j]->assignment.variables.size()
					|| !block[assignmentIndex]->assignment.variables.size()
					|| !block[j]->assignment.variables.back().slotScope
					|| !block[assignmentIndex]->assignment.variables.back().slotScope) continue;
				if (*block[j]->assignment.variables.back().slotScope != *block[assignmentIndex]->assignment.variables.back().slotScope) {
					(*block[assignmentIndex]->assignment.variables.back().slotScope)->usages += (*block[j]->assignment.variables.back().slotScope)->usages;
					if ((*block[j]->assignment.variables.back().slotScope)->scopeBegin < (*block[assignmentIndex]->assignment.variables.back().slotScope)->scopeBegin)
						(*block[assignmentIndex]->assignment.variables.back().slotScope)->scopeBegin = (*block[j]->assignment.variables.back().slotScope)->scopeBegin;
					if ((*block[j]->assignment.variables.back().slotScope)->scopeEnd > (*block[assignmentIndex]->assignment.variables.back().slotScope)->scopeEnd)
						(*block[assignmentIndex]->assignment.variables.back().slotScope)->scopeEnd = (*block[j]->assignment.variables.back().slotScope)->scopeEnd;
					*block[j]->assignment.variables.back().slotScope = *block[assignmentIndex]->assignment.variables.back().slotScope;
					if (block[j]->assignment.variables.back().slotScope != block[assignmentIndex]->assignment.variables.back().slotScope)
						function.slotScopeCollector.remove_scope(block[j]->assignment.variables.back().slot, block[j]->assignment.variables.back().slotScope);
				}

				continue;
			case AST_STATEMENT_GOTO:
			case AST_STATEMENT_BREAK:
				function.remove_jump(block[j]->instruction.id, block[j]->instruction.target);
				continue;
			}
		}

		block[i] = block[assignmentIndex];
		block[i]->type = AST_STATEMENT_ASSIGNMENT;
		block[i]->instruction.label = block[index]->instruction.label;
		if ((*block[i]->assignment.variables.back().slotScope)->scopeBegin >= block[index]->instruction.id) block[i]->assignment.forwardDeclaration = true;
		block.erase(block.begin() + index, block.begin() + i);
		i = index;
	}

	for (uint32_t i = block.size(); i--;) {
		switch (block[i]->type) {
		case AST_STATEMENT_CONDITION:
			blockInfo.index = i;
			targetLabel = get_label_from_next_statement(function, blockInfo, true, false);
			targetIndex = INVALID_ID;
			index = i;

			while (index && block[index - 1]->type == AST_STATEMENT_CONDITION) {
				index--;
			}

			for (uint32_t j = index; j <= i; j++) {
				if (function.is_valid_label(block[j]->instruction.label)) {
					if (function.labels[block[j]->instruction.label].jumpIds.front() < block[index]->instruction.id
						|| function.labels[block[j]->instruction.label].jumpIds.back() > block[j]->instruction.id) {
						index = j;
						targetIndex = INVALID_ID;
					} else if ((j
						&& j - 1 >= index
						&& block[j - 1]->instruction.target == function.labels[block[j]->instruction.label].target)) {
						for (uint32_t k = index; k < j
							&& block[k]->instruction.target > block[k]->instruction.id
							&& block[k]->instruction.target <= block[j]->instruction.id; k++) {
							if (k != j - 1) continue;
							index = j;
							targetIndex = INVALID_ID;
							break;
						}
					}
				}

				if ((targetLabel == INVALID_ID
					|| block[j]->instruction.target != function.labels[targetLabel].target)
					&& (block[j]->instruction.target < block[j]->instruction.id
						|| block[j]->instruction.target > block[i]->instruction.id)) {
					if (targetIndex != INVALID_ID) {
						if (block[j]->instruction.target == block[targetIndex]->instruction.target) continue;
						index = targetIndex + 1;
						j = targetIndex;
						targetIndex = INVALID_ID;
						continue;
					}

					targetIndex = j;
				}
			}

			if (targetIndex == INVALID_ID) {
				extendedTargetLabel = targetLabel;
				targetLabel = INVALID_ID;
			} else {
				extendedTargetLabel = function.get_label_from_id(block[targetIndex]->instruction.target);
			}

			{
				ConditionBuilder conditionBuilder(ConditionBuilder::STATEMENT, *this, INVALID_ID, targetLabel, extendedTargetLabel);

				for (uint32_t j = index; j <= i; j++) {
					if (block[j]->assignment.variables.size()) {
						// 某些字节码变体的 `x = a or b` 拷贝条件与标准布局不同,
						// 无法消除时退化为纯测试条件 (丢失拷贝语义)。
						block[j]->assignment.variables.clear();
					}
					conditionBuilder.add_node(conditionBuilder.get_node_type(block[j]->instruction.type, block[j]->condition.swapped),
						block[j]->instruction.label, function.get_label_from_id(block[j]->instruction.target), &block[j]->assignment.expressions);
				}

				expressions.back() = conditionBuilder.build_condition();
				assert(expressions.back(), "Failed to build condition", bytecode.filePath, DEBUG_INFO);
				block[i]->assignment.expressions = expressions;

				for (uint32_t j = index; j <= i; j++) {
					function.remove_jump(block[j]->instruction.id + 1, block[j]->instruction.target);
				}

				block[i]->instruction.target = function.labels[extendedTargetLabel].target;
				function.add_jump(block[i]->instruction.id, block[i]->instruction.target);
				block[i]->instruction.label = block[index]->instruction.label;
				block.erase(block.begin() + index, block.begin() + i);
				i = index;
			}

			if (i
				&& block[i]->instruction.type == Bytecode::BC_OP_JMP
				&& block[i]->assignment.expressions.back()->type == AST_EXPRESSION_CONSTANT
				&& block[i]->assignment.expressions.back()->constant->type == AST_CONSTANT_FALSE
				&& !function.is_valid_label(block[i]->instruction.label)
				&& block[i - 1]->type == AST_STATEMENT_ASSIGNMENT
				&& block[i - 1]->assignment.variables.size() == 1
				&& block[i - 1]->assignment.variables.back().type == AST_VARIABLE_SLOT
				&& block[i - 1]->assignment.expressions.size() == 1
				&& get_constant_type(block[i - 1]->assignment.expressions.back())) {
				//TODO
				function.remove_jump(block[i]->instruction.id, block[i]->instruction.target);
				block[i]->assignment.expressions.clear();
				block[i]->type = AST_STATEMENT_GOTO;
				block.emplace(block.begin() + i, new_statement(AST_STATEMENT_GOTO));
				block[i]->instruction.type = Bytecode::BC_OP_JMP;
				block[i]->instruction.id = block[i + 1]->instruction.id;
				block[i + 1]->instruction.id++;
				block[i]->instruction.target = block[i + 1]->instruction.id;
				function.add_jump(block[i]->instruction.id, block[i]->instruction.target);
				function.add_jump(block[i + 1]->instruction.id, block[i + 1]->instruction.target);
				block[i + 1]->instruction.label = function.get_label_from_id(block[i + 1]->instruction.id);
			}

			continue;
		case AST_STATEMENT_NUMERIC_FOR:
		case AST_STATEMENT_GENERIC_FOR:
			eliminate_conditions(function, block[i]->block, nullptr);
			continue;
		case AST_STATEMENT_LOOP:
		case AST_STATEMENT_DECLARATION:
			blockInfo.index = i;
			eliminate_conditions(function, block[i]->block, &blockInfo);
			continue;
		}
	}

	return build_multi_assignment(function, block);
}

void Ast::build_multi_assignment(Function& function, std::vector<Statement*>& block) {
	bool isMultiAssignment;
	uint32_t index;

	for (uint32_t i = block.size(); i--;) {
		switch (block[i]->type) {
		case AST_STATEMENT_ASSIGNMENT:
			if (block[i]->assignment.variables.size() >= 2) {
				if (i + block[i]->assignment.variables.size() >= block.size()) continue;
				isMultiAssignment = true;

				for (uint8_t j = block[i]->assignment.variables.size(); j--;) {
					if ((*block[i]->assignment.variables[j].slotScope)->usages == 1
						&& !function.is_valid_label(block[i + block[i]->assignment.variables.size() - j]->instruction.label)
						&& block[i + block[i]->assignment.variables.size() - j]->type == AST_STATEMENT_ASSIGNMENT
						&& block[i + block[i]->assignment.variables.size() - j]->assignment.variables.size() == 1
						&& (block[i + block[i]->assignment.variables.size() - j]->assignment.variables.back().type != AST_VARIABLE_TABLE_INDEX
							|| (block[i + block[i]->assignment.variables.size() - j]->assignment.variables.back().table->type == AST_EXPRESSION_VARIABLE
								&& block[i + block[i]->assignment.variables.size() - j]->assignment.variables.back().table->variable->type == AST_VARIABLE_SLOT
								&& (get_constant_type(block[i + block[i]->assignment.variables.size() - j]->assignment.variables.back().tableIndex)
									|| (block[i + block[i]->assignment.variables.size() - j]->assignment.variables.back().tableIndex->type == AST_EXPRESSION_VARIABLE
										&& block[i + block[i]->assignment.variables.size() - j]->assignment.variables.back().tableIndex->variable->type == AST_VARIABLE_SLOT))))
						&& block[i + block[i]->assignment.variables.size() - j]->assignment.expressions.size() == 1
						&& block[i + block[i]->assignment.variables.size() - j]->assignment.expressions.back()->type == AST_EXPRESSION_VARIABLE
						&& block[i + block[i]->assignment.variables.size() - j]->assignment.expressions.back()->variable->type == AST_VARIABLE_SLOT
						&& block[i + block[i]->assignment.variables.size() - j]->assignment.expressions.back()->variable->slotScope == block[i]->assignment.variables[j].slotScope)
						continue;
					isMultiAssignment = false;
					break;
				}

				if (!isMultiAssignment) continue;

				for (uint8_t j = block[i]->assignment.variables.size(); j--;) {
					function.slotScopeCollector.remove_scope(block[i]->assignment.variables[j].slot, block[i]->assignment.variables[j].slotScope);
					// 把旧结果作用域重定向到新变量作用域: 折叠/内联产生的既有引用
					// (如 `if file == nil` 的测试操作数) 仍指向旧作用域对象, 否则
					// 会留下无名幽灵作用域, 写出阶段退化为 `var_<槽位>`。
					SlotScope* oldResultScope = *block[i]->assignment.variables[j].slotScope;
					SlotScope** newVariableScopePtr = block[i + 1]->assignment.variables.back().slotScope;
					SlotScope* newVariableScope = newVariableScopePtr ? *newVariableScopePtr : nullptr;
					if (oldResultScope && newVariableScope && oldResultScope != newVariableScope) {
						oldResultScope->mergedScopes.emplace_back(&oldResultScope->slotScope);
						oldResultScope->slotScope = newVariableScope;
					}
					block[i]->assignment.variables[j] = block[i + 1]->assignment.variables.back();
					block.erase(block.begin() + i + 1);
				}

				break;
			}
		case AST_STATEMENT_FUNCTION_CALL:
			index = i;

			if (block[i]->type == AST_STATEMENT_FUNCTION_CALL
				|| (block[i]->assignment.variables.back().type == AST_VARIABLE_SLOT
					&& !(*block[i]->assignment.variables.back().slotScope)->usages
					&& !block[i]->assignment.forwardDeclaration)) {
				while (index
					&& !function.is_valid_label(block[index]->instruction.label)
					&& block[index - 1]->type == AST_STATEMENT_ASSIGNMENT
					&& block[index - 1]->assignment.variables.size() == 1
					&& block[index - 1]->assignment.variables.back().type == AST_VARIABLE_SLOT
					&& !(*block[index - 1]->assignment.variables.back().slotScope)->usages
					&& !block[index - 1]->assignment.forwardDeclaration) {
					index--;
				}
			}

			if (index
				&& i + 1 < block.size()
				&& !function.is_valid_label(block[index]->instruction.label)
				&& !function.is_valid_label(block[i + 1]->instruction.label)
				&& block[index - 1]->type == AST_STATEMENT_ASSIGNMENT
				&& block[index - 1]->assignment.variables.size() == 1
				&& block[index - 1]->assignment.variables.back().type == AST_VARIABLE_SLOT
				&& (*block[index - 1]->assignment.variables.back().slotScope)->usages == 1
				&& block[i + 1]->type == AST_STATEMENT_ASSIGNMENT
				&& block[i + 1]->assignment.variables.size() == 1
				&& (block[i + 1]->assignment.variables.back().type != AST_VARIABLE_TABLE_INDEX
					|| (block[i + 1]->assignment.variables.back().table->type == AST_EXPRESSION_VARIABLE
						&& block[i + 1]->assignment.variables.back().table->variable->type == AST_VARIABLE_SLOT
						&& block[i + 1]->assignment.variables.back().tableIndex
						&& (get_constant_type(block[i + 1]->assignment.variables.back().tableIndex)
							|| (block[i + 1]->assignment.variables.back().tableIndex->type == AST_EXPRESSION_VARIABLE
								&& block[i + 1]->assignment.variables.back().tableIndex->variable->type == AST_VARIABLE_SLOT))))
				&& block[i + 1]->assignment.expressions.size() == 1
				&& block[i + 1]->assignment.expressions.back()->type == AST_EXPRESSION_VARIABLE
				&& block[i + 1]->assignment.expressions.back()->variable->type == AST_VARIABLE_SLOT
				&& block[i + 1]->assignment.expressions.back()->variable->slotScope == block[index - 1]->assignment.variables.back().slotScope) {
				if (block[i]->type == AST_STATEMENT_ASSIGNMENT) {
					switch (block[i]->assignment.variables.back().type) {
					case AST_VARIABLE_SLOT:
						if (!(*block[i]->assignment.variables.back().slotScope)->usages && !block[i]->assignment.forwardDeclaration) {
							function.slotScopeCollector.remove_scope(block[i]->assignment.variables.back().slot, block[i]->assignment.variables.back().slotScope);
							block[i]->assignment.variables.clear();
						}

						break;
					case AST_VARIABLE_TABLE_INDEX:
						if (index == i
							&& (block[i]->assignment.variables.back().table->type != AST_EXPRESSION_VARIABLE
								|| block[i]->assignment.variables.back().table->variable->type != AST_VARIABLE_SLOT
								|| (!get_constant_type(block[i]->assignment.variables.back().tableIndex)
									&& (block[i]->assignment.variables.back().tableIndex->type != AST_EXPRESSION_VARIABLE
										|| block[i]->assignment.variables.back().tableIndex->variable->type != AST_VARIABLE_SLOT))))
							continue;
						break;
					}
				} else {
					block[i]->type = AST_STATEMENT_ASSIGNMENT;
				}

				while (i != index) {
					i--;
					function.slotScopeCollector.remove_scope(block[i]->assignment.variables.back().slot, block[i]->assignment.variables.back().slotScope);
					block[i + 1]->assignment.expressions.emplace(block[i + 1]->assignment.expressions.begin(), block[i]->assignment.expressions.back());
					block.erase(block.begin() + i);
				}

				break;
			}

			if (block[i]->type == AST_STATEMENT_FUNCTION_CALL && block[i]->assignment.expressions.back()->type == AST_EXPRESSION_VARARG) {
				assert(i
					&& !function.is_valid_label(block[i]->instruction.label)
					&& block[i - 1]->type == AST_STATEMENT_ASSIGNMENT
					&& block[i - 1]->assignment.variables.size() == 1,
					"Unable to eliminate vararg with zero returns", bytecode.filePath, DEBUG_INFO);
				block[i - 1]->assignment.expressions.emplace_back(block[i]->assignment.expressions.back());
				block.erase(block.begin() + i);
				i--;
			}
		default:
			continue;
		}

		while (i
			&& i + 1 < block.size()
			&& !function.is_valid_label(block[i]->instruction.label)
			&& !function.is_valid_label(block[i + 1]->instruction.label)
			&& block[i - 1]->type == AST_STATEMENT_ASSIGNMENT
			&& block[i - 1]->assignment.variables.size() == 1
			&& block[i - 1]->assignment.variables.back().type == AST_VARIABLE_SLOT
			&& (*block[i - 1]->assignment.variables.back().slotScope)->usages == 1
			&& block[i + 1]->type == AST_STATEMENT_ASSIGNMENT
			&& block[i + 1]->assignment.variables.size() == 1
			&& (block[i + 1]->assignment.variables.back().type != AST_VARIABLE_TABLE_INDEX
				|| (block[i + 1]->assignment.variables.back().table->type == AST_EXPRESSION_VARIABLE
					&& block[i + 1]->assignment.variables.back().table->variable->type == AST_VARIABLE_SLOT
					&& (get_constant_type(block[i + 1]->assignment.variables.back().tableIndex)
						|| (block[i + 1]->assignment.variables.back().tableIndex->type == AST_EXPRESSION_VARIABLE
							&& block[i + 1]->assignment.variables.back().tableIndex->variable->type == AST_VARIABLE_SLOT))))
			&& block[i + 1]->assignment.expressions.size() == 1
			&& block[i + 1]->assignment.expressions.back()->type == AST_EXPRESSION_VARIABLE
			&& block[i + 1]->assignment.expressions.back()->variable->type == AST_VARIABLE_SLOT
			&& block[i + 1]->assignment.expressions.back()->variable->slotScope == block[i - 1]->assignment.variables.back().slotScope) {
			function.slotScopeCollector.remove_scope(block[i - 1]->assignment.variables.back().slot, block[i - 1]->assignment.variables.back().slotScope);
			block[i]->assignment.expressions.emplace(block[i]->assignment.expressions.begin(), block[i - 1]->assignment.expressions.back());
			block[i]->assignment.variables.emplace(block[i]->assignment.variables.begin(), block[i + 1]->assignment.variables.back());
			block[i]->instruction.label = block[i - 1]->instruction.label;
			block.erase(block.begin() + i - 1);
			block.erase(block.begin() + i);
			i--;
		}

		for (uint8_t j = block[i]->assignment.variables.size(); j--
			&& i
			&& !function.is_valid_label(block[i]->instruction.label)
			&& block[i - 1]->type == AST_STATEMENT_ASSIGNMENT
			&& block[i - 1]->assignment.variables.size() == 1
			&& block[i - 1]->assignment.variables.back().type == AST_VARIABLE_SLOT
			&& (*block[i - 1]->assignment.variables.back().slotScope)->usages == 1;) {
			if (block[i]->assignment.variables[j].type != AST_VARIABLE_TABLE_INDEX) continue;

			if (block[i]->assignment.variables[j].tableIndex->type == AST_EXPRESSION_VARIABLE
				&& block[i]->assignment.variables[j].tableIndex->variable->type == AST_VARIABLE_SLOT
				&& block[i]->assignment.variables[j].tableIndex->variable->slotScope == block[i - 1]->assignment.variables.back().slotScope) {
				function.slotScopeCollector.remove_scope(block[i - 1]->assignment.variables.back().slot, block[i - 1]->assignment.variables.back().slotScope);
				block[i]->assignment.variables[j].tableIndex = block[i - 1]->assignment.expressions.back();
				block[i]->instruction.label = block[i - 1]->instruction.label;
				i--;
				block.erase(block.begin() + i);
				j++;
				continue;
			}

			if (block[i]->assignment.variables[j].table->type == AST_EXPRESSION_VARIABLE && block[i]->assignment.variables[j].table->variable->slotScope == block[i - 1]->assignment.variables.back().slotScope) {
				function.slotScopeCollector.remove_scope(block[i - 1]->assignment.variables.back().slot, block[i - 1]->assignment.variables.back().slotScope);
				block[i]->assignment.variables[j].table = block[i - 1]->assignment.expressions.back();
				block[i]->instruction.label = block[i - 1]->instruction.label;
				i--;
				block.erase(block.begin() + i);
			}
		}
	}
}

void Ast::build_if_statements_from_map(Function& function, std::vector<Statement*>& block, BlockInfo* const& previousBlock, std::unordered_map<Statement*, uint32_t>& offsetMap) {
	BlockInfo blockInfo = { .block = block, .previousBlock = previousBlock };
	uint32_t index;

	for (uint32_t i = 0; i < block.size(); i++) {
		switch (block[i]->type) {
		case AST_STATEMENT_GOTO:
			if (!offsetMap.contains(block[i])) continue;
		case AST_STATEMENT_CONDITION:
			function.remove_jump(block[i]->instruction.id, block[i]->instruction.target);
			index = offsetMap[block[i]] + i;

			if (block[i]->type == AST_STATEMENT_GOTO && block[i]->instruction.type == Bytecode::BC_OP_JMP) {
				block[i]->type = AST_STATEMENT_EMPTY;
				i++;
				index++;
				block.emplace(block.begin() + i, new_statement(AST_STATEMENT_GOTO));
				block[i]->instruction.id = block[i - 1]->instruction.id;
				block[i - 1]->instruction.id = INVALID_ID;
			}

			block[i]->block.reserve(index - i);
			block[i]->block.insert(block[i]->block.begin(), block.begin() + i + 1, block.begin() + index + 1);
			block.erase(block.begin() + i + 1, block.begin() + index + 1);

			if (block[i]->type == AST_STATEMENT_CONDITION
				&& block[i]->block.size()
				&& block[i]->block.back()->type == AST_STATEMENT_GOTO
				&& block[i]->block.back()->instruction.type != Bytecode::BC_OP_LOOP) {
				index = offsetMap[block[i]->block.back()] + i;
				block.emplace(block.begin() + i + 1, new_statement(AST_STATEMENT_ELSE));
				block[i + 1]->block.reserve(index - i);
				block[i + 1]->block.insert(block[i + 1]->block.begin(), block.begin() + i + 2, block.begin() + index + 2);
				block.erase(block.begin() + i + 2, block.begin() + index + 2);
				function.remove_jump(block[i]->block.back()->instruction.id, block[i]->block.back()->instruction.target);
				block[i]->block.back()->type = AST_STATEMENT_EMPTY;
				blockInfo.index = i + 1;
				build_if_statements_from_map(function, block[i + 1]->block, &blockInfo, offsetMap);
			}

			if (block[i]->type == AST_STATEMENT_GOTO) block[i]->assignment.expressions.emplace_back(new_primitive(1));
			block[i]->type = AST_STATEMENT_IF;
			blockInfo.index = i;
			build_if_statements_from_map(function, block[i]->block, &blockInfo, offsetMap);
			continue;
		case AST_STATEMENT_NUMERIC_FOR:
		case AST_STATEMENT_GENERIC_FOR:
			build_if_statements(function, block[i]->block, nullptr);
			continue;
		case AST_STATEMENT_LOOP:
		case AST_STATEMENT_DECLARATION:
			blockInfo.index = i;
			build_if_statements(function, block[i]->block, &blockInfo);
			continue;
		}
	}
}

void Ast::build_if_statements(Function& function, std::vector<Statement*>& block, BlockInfo* const& previousBlock) {
	const auto build_if_false_statements = [&](std::vector<Statement*>& block, BlockInfo* const& previousBlock)->void {
		BlockInfo blockInfo = { .block = block, .previousBlock = previousBlock };
		uint32_t index, targetLabel;

		for (uint32_t i = block.size(); i--;) {
			if (block[i]->type != AST_STATEMENT_GOTO || block[i]->instruction.type == Bytecode::BC_OP_LOOP) continue;
			targetLabel = INVALID_ID;

			for (index = i; index < block.size(); index++) {
				blockInfo.index = index;
				targetLabel = get_label_from_next_statement(function, blockInfo, false, false);
				if (targetLabel == INVALID_ID || function.labels[targetLabel].target != block[i]->instruction.target) targetLabel = get_label_from_next_statement(function, blockInfo, true, false);
				if (targetLabel == INVALID_ID) continue;
				if (function.labels[targetLabel].target == block[i]->instruction.target && is_valid_block(function, blockInfo, block[i]->instruction.id + 1)) break;
				targetLabel = INVALID_ID;
			}

			if (targetLabel == INVALID_ID) continue;
			function.remove_jump(block[i]->instruction.id, block[i]->instruction.target);

			if (block[i]->instruction.type == Bytecode::BC_OP_JMP) {
				block[i]->type = AST_STATEMENT_EMPTY;
				i++;
				index++;
				block.emplace(block.begin() + i, new_statement(AST_STATEMENT_IF));
				block[i]->instruction.id = block[i - 1]->instruction.id;
				block[i - 1]->instruction.id = INVALID_ID;
			} else {
				block[i]->type = AST_STATEMENT_IF;
			}

			block[i]->assignment.expressions.emplace_back(new_primitive(1));
			block[i]->block.reserve(index - i);
			block[i]->block.insert(block[i]->block.begin(), block.begin() + i + 1, block.begin() + index + 1);
			block.erase(block.begin() + i + 1, block.begin() + index + 1);
		}
	};

	const auto build_else_statements = [&](std::vector<Statement*>& block, BlockInfo* const& previousBlock)->void {
		BlockInfo blockInfo = { .block = block, .previousBlock = previousBlock };
		uint32_t index, targetLabel;

		for (uint32_t i = block.size(); i--;) {
			if (block[i]->type != AST_STATEMENT_IF) continue;

			if (block[i]->block.size()
				&& block[i]->block.back()->type == AST_STATEMENT_GOTO
				&& block[i]->block.back()->instruction.type != Bytecode::BC_OP_LOOP) {
				targetLabel = INVALID_ID;

				for (index = i; index < block.size(); index++) {
					blockInfo.index = index;
					targetLabel = get_label_from_next_statement(function, blockInfo, false, false);
					if (targetLabel == INVALID_ID || function.labels[targetLabel].target != block[i]->block.back()->instruction.target) targetLabel = get_label_from_next_statement(function, blockInfo, true, false);
					if (targetLabel == INVALID_ID) continue;
					if (function.labels[targetLabel].target == block[i]->block.back()->instruction.target && is_valid_block(function, blockInfo, block[i]->block.back()->instruction.id + 1)) break;
					targetLabel = INVALID_ID;
				}

				if (targetLabel != INVALID_ID) {
					block.emplace(block.begin() + i + 1, new_statement(AST_STATEMENT_ELSE));
					block[i + 1]->block.reserve(index - i);
					block[i + 1]->block.insert(block[i + 1]->block.begin(), block.begin() + i + 2, block.begin() + index + 2);
					block.erase(block.begin() + i + 2, block.begin() + index + 2);
					function.remove_jump(block[i]->block.back()->instruction.id, block[i]->block.back()->instruction.target);
					block[i]->block.back()->type = AST_STATEMENT_EMPTY;
					blockInfo.index = i + 1;
					build_if_false_statements(block[i + 1]->block, &blockInfo);
					build_if_false_statements(block[i]->block, nullptr);
					continue;
				}
			}

			blockInfo.index = i;
			build_if_false_statements(block[i]->block, &blockInfo);
		}
	};

	BlockInfo blockInfo = { .block = block, .previousBlock = previousBlock };
	uint32_t index, targetLabel;
	std::vector<uint32_t> indexes;
	std::unordered_map<Statement*, uint32_t> offsetMap;

	for (uint32_t i = 0; i < block.size(); i++) {
		if (indexes.size()
			&& (i == indexes.back()
				|| block[i]->type != AST_STATEMENT_CONDITION)) {
			blockInfo.index = i;
			targetLabel = get_label_from_next_statement(function, blockInfo, false, false);
			if (targetLabel == INVALID_ID || function.labels[targetLabel].target != block[indexes.back()]->instruction.target) targetLabel = get_label_from_next_statement(function, blockInfo, true, false);

			if (targetLabel != INVALID_ID
				&& function.labels[targetLabel].target == block[indexes.back()]->instruction.target
				&& is_valid_block(function, blockInfo, block[indexes.back()]->instruction.id + (block[indexes.back()]->type == AST_STATEMENT_CONDITION ? 2 : 1))) {
				offsetMap.emplace(block[indexes.back()], i - indexes.back());

				if (i - indexes.back()
					&& block[indexes.back()]->type == AST_STATEMENT_CONDITION
					&& block[i]->type == AST_STATEMENT_GOTO
					&& block[i]->instruction.type != Bytecode::BC_OP_LOOP) {
					function.remove_jump(block[indexes.back()]->instruction.id, block[indexes.back()]->instruction.target);
					indexes.emplace_back(i);
					i--;
					continue;
				}

				if (indexes.size() >= 2 && offsetMap.contains(block[indexes[indexes.size() - 2]])) {
					indexes.pop_back();
					function.add_jump(block[indexes.back()]->instruction.id, block[indexes.back()]->instruction.target);
				}

				indexes.pop_back();
				i--;
				continue;
			}

			if (i == indexes.back()) continue;
		}

		switch (block[i]->type) {
		case AST_STATEMENT_GOTO:
			if (block[i]->instruction.type == Bytecode::BC_OP_LOOP) continue;
		case AST_STATEMENT_CONDITION:
			if (offsetMap.contains(block[i])) continue;
			indexes.emplace_back(i);
			i--;
		}
	}

	if (indexes.size() == 1
		&& block[indexes.back()]->type == AST_STATEMENT_GOTO
		&& indexes.back() == block.size() - 1
		&& previousBlock
		&& previousBlock->block[previousBlock->index]->type == AST_STATEMENT_LOOP)
		indexes.pop_back();
	if (!indexes.size()) return build_if_statements_from_map(function, block, previousBlock, offsetMap);

	for (uint32_t i = indexes.size(); i--;) {
		if (offsetMap.contains(block[indexes[i]])) function.add_jump(block[indexes[i]]->instruction.id, block[indexes[i]]->instruction.target);
	}

	for (uint32_t i = block.size(); i--;) {
		switch (block[i]->type) {
		case AST_STATEMENT_CONDITION:
			block[i]->type = AST_STATEMENT_IF;
			targetLabel = INVALID_ID;

			for (index = i; index < block.size(); index++) {
				blockInfo.index = index;
				targetLabel = get_label_from_next_statement(function, blockInfo, false, false);
				if (targetLabel == INVALID_ID || function.labels[targetLabel].target != block[i]->instruction.target) targetLabel = get_label_from_next_statement(function, blockInfo, true, false);
				if (targetLabel == INVALID_ID) continue;
				if (function.labels[targetLabel].target == block[i]->instruction.target && is_valid_block(function, blockInfo, block[i]->instruction.id + 2)) break;
				targetLabel = INVALID_ID;
			}

			if (targetLabel == INVALID_ID) {
				// 某些字节码变体的条件目标常只由条件自身注册为标签 (JMP 载体被
				// group_jumps 合并), 标准路径找不到"下一语句"标签; 回退为
				// 直接按目标地址查找标签。
				targetLabel = function.get_label_from_id(block[i]->instruction.target);
				if (targetLabel != INVALID_ID && !function.is_valid_label(targetLabel)) targetLabel = INVALID_ID;

				if (targetLabel != INVALID_ID) {
					// 标准路径失败时 index 会停在 block.size(), 直接用会越界;
					// 按目标地址/标签重新定位承载语句, 其前一条语句作为 if 体终点。
					index = block.size() - 1;

					for (uint32_t j = i + 1; j < block.size(); j++) {
						if (block[j]->instruction.id == block[i]->instruction.target
							|| block[j]->instruction.label == targetLabel) {
							index = j - 1;
							break;
						}
					}

					if (index < i) index = i;
				}
			}

			assert(targetLabel != INVALID_ID, "Failed to build if statement", bytecode.filePath, DEBUG_INFO);
			block[i]->block.reserve(index - i);
			block[i]->block.insert(block[i]->block.begin(), block.begin() + i + 1, block.begin() + index + 1);
			block.erase(block.begin() + i + 1, block.begin() + index + 1);
			function.remove_jump(block[i]->instruction.id, block[i]->instruction.target);
			blockInfo.index = i;
			build_else_statements(block[i]->block, &blockInfo);
			continue;
		case AST_STATEMENT_NUMERIC_FOR:
		case AST_STATEMENT_GENERIC_FOR:
			build_if_statements(function, block[i]->block, nullptr);
			continue;
		case AST_STATEMENT_LOOP:
		case AST_STATEMENT_DECLARATION:
			blockInfo.index = i;
			build_if_statements(function, block[i]->block, &blockInfo);
			continue;
		}
	}

	build_else_statements(block, previousBlock);
	build_if_false_statements(block, previousBlock);
}
