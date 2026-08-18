#include "../main.h"

// 各 pass 之间共享的内部辅助函数 (声明; 定义在 ast/eliminate.cpp)。
// 注意: 本头文件不包含 main.h, 引用前必须已包含之。


#include "ast_internal.h"

void Ast::clean_up(Function& function) {
	if (function.hasDebugInfo) {
		for (uint32_t i = function.parameterNames.size(); i--;) {
			(*function.slotScopeCollector.slotInfos[i].activeSlotScope)->name = function.parameterNames[i];
		}
	} else {
		function.parameterNames.resize(function.prototype.header.parameters);

		for (uint32_t i = function.parameterNames.size(); i--;) {
			function.parameterNames[i] = "arg_" + std::to_string(minimizeDiffs ? function.level : function.id) + "_" + std::to_string(i);
			(*function.slotScopeCollector.slotInfos[i].activeSlotScope)->name = function.parameterNames[i];
		}
	}

	optimize_conditional_assignments(function);
	uint32_t variableCounter = 0, iteratorCounter = 0;
	clean_up_block(function, function.block, variableCounter, iteratorCounter, nullptr);
	optimize_conditional_assignments(function);
	cleanup_unused_declarations(function, function.block);
	fix_out_of_scope_declarations(function);

	// 兜底: 任何残留的无名槽位作用域 (写入者被极端情况消除后仍有引用),
	// 补名并确保函数头部存在声明, 避免写出未声明的 `var_<槽位>`。
	std::unordered_map<SlotScope*, uint8_t> unnamedScopes;
	{
		const auto collectUnnamed = [&](const auto& self, const Expression* expression) -> void {
			if (!expression) return;
			switch (expression->type) {
			case AST_EXPRESSION_VARIABLE:
				if (expression->variable->type == AST_VARIABLE_SLOT
					&& expression->variable->slotScope
					&& *expression->variable->slotScope
					&& (*expression->variable->slotScope)->name.empty()) {
					unnamedScopes.try_emplace(*expression->variable->slotScope, expression->variable->slot);
				}
				if (expression->variable->table) self(self, expression->variable->table);
				if (expression->variable->tableIndex) self(self, expression->variable->tableIndex);
				break;
			case AST_EXPRESSION_FUNCTION_CALL:
				self(self, expression->functionCall->function);
				for (const Expression* arg : expression->functionCall->arguments) self(self, arg);
				if (expression->functionCall->multresArgument) self(self, expression->functionCall->multresArgument);
				break;
			case AST_EXPRESSION_TABLE:
				for (const auto& field : expression->table->fields) {
					self(self, field.key);
					self(self, field.value);
				}
				if (expression->table->multresField) self(self, expression->table->multresField);
				break;
			case AST_EXPRESSION_BINARY_OPERATION:
				self(self, expression->binaryOperation->leftOperand);
				self(self, expression->binaryOperation->rightOperand);
				break;
			case AST_EXPRESSION_UNARY_OPERATION:
				self(self, expression->unaryOperation->operand);
				break;
			default: break;
			}
		};
		const auto walk = [&](const auto& self, const Statement* statement) -> void {
			if (!statement) return;
			for (const Expression* expr : statement->assignment.expressions) collectUnnamed(collectUnnamed, expr);
			if (statement->assignment.multresReturn) collectUnnamed(collectUnnamed, statement->assignment.multresReturn);
			for (const Variable& variable : statement->assignment.variables) {
				collectUnnamed(collectUnnamed, variable.table);
				collectUnnamed(collectUnnamed, variable.tableIndex);
			}
			for (const Statement* child : statement->block) self(self, child);
		};
		const auto walkRoot = [&](const auto& self, std::vector<Statement*>& block) -> void {
			for (Statement* statement : block) {
				if (statement) {
					walk(walk, statement);
					if (!statement->block.empty()) self(self, statement->block);
				}
			}
		};
		walkRoot(walkRoot, function.block);
	}

	if (!unnamedScopes.empty()) {
		uint32_t counter = 9000;
		std::unordered_set<SlotScope*> declaredScopes;
		{
			const auto collectDeclared = [&](const auto& self, const Statement* statement) -> void {
				if (!statement) return;
				for (const Variable& variable : statement->assignment.variables) {
					if (variable.type == AST_VARIABLE_SLOT && variable.slotScope && *variable.slotScope) {
						declaredScopes.insert(*variable.slotScope);
					}
				}
				for (const Statement* child : statement->block) self(self, child);
			};
			const auto walkRoot = [&](const auto& self, std::vector<Statement*>& block) -> void {
				for (Statement* statement : block) {
					if (statement) {
						collectDeclared(collectDeclared, statement);
						if (!statement->block.empty()) self(self, statement->block);
					}
				}
			};
			walkRoot(walkRoot, function.block);
		}

		for (const auto& [scope, slot] : unnamedScopes) {
			scope->name = "var_" + std::to_string(minimizeDiffs ? function.level : function.id)
				+ "_" + std::to_string(counter++);
			scope->isSynthetic = true;

			if (declaredScopes.contains(scope)) continue;
			Statement* declaration = new_statement(AST_STATEMENT_DECLARATION);
			declaration->assignment.variables.emplace_back();
			declaration->assignment.variables.back().type = AST_VARIABLE_SLOT;
			declaration->assignment.variables.back().slot = slot;
			declaration->assignment.variables.back().slotScope = scope->slotScope ? &scope->slotScope : nullptr;
			function.block.insert(function.block.begin(), declaration);
			declaredScopes.insert(scope);
		}
	}

	for (uint32_t i = 0, labelCounter = 0; i < function.labels.size(); i++) {
		if (!function.labels[i].jumpIds.size()) continue;
		function.labels[i].name = "label_" + std::to_string(minimizeDiffs ? function.level : function.id) + "_" + std::to_string(labelCounter);
		labelCounter++;
	}

}

void Ast::fixup_labels(Function& function) {
	std::unordered_map<Statement*, Statement*> parent;
	std::unordered_map<uint32_t, Statement*> labelStatements;
	std::unordered_map<uint32_t, std::vector<Statement*>> gotoStatements;

	std::vector<std::pair<std::vector<Statement*>*, Statement*>> stack;
	stack.push_back({ &function.block, nullptr });

	while (!stack.empty()) {
		const auto [block, owner] = stack.back();
		stack.pop_back();

		for (Statement* statement : *block) {
			if (!statement) continue;
			parent[statement] = owner;

			if (statement->type == AST_STATEMENT_LABEL) {
				const uint32_t label = statement->instruction.label;
				if (function.is_valid_label(label)) labelStatements[label] = statement;
			} else if (statement->type == AST_STATEMENT_GOTO) {
				const uint32_t label = statement->instruction.label;
				if (function.is_valid_label(label)) gotoStatements[label].push_back(statement);
			}

			if (!statement->block.empty()) stack.push_back({ &statement->block, statement });
		}
	}

	const auto isDescendant = [&](Statement* ancestor, Statement* target) -> bool {
		for (Statement* current = target; current; current = parent[current]) {
			if (current == ancestor) return true;
		}
		return false;
	};

	for (const auto& [label, labelStatement] : labelStatements) {
		const auto gotoIt = gotoStatements.find(label);
		if (gotoIt == gotoStatements.end() || gotoIt->second.empty()) continue;
		const auto& gotos = gotoIt->second;

		Statement* labelBlock = parent[labelStatement];
		// 计算所有引用该 label 的 GOTO 以及 label 自身所在块的最近公共祖先块。
		Statement* commonBlock = parent[gotos.front()];
		while (commonBlock && !isDescendant(commonBlock, labelBlock)) {
			commonBlock = parent[commonBlock];
		}
		for (size_t i = 1; i < gotos.size(); i++) {
			Statement* gotoBlock = parent[gotos[i]];
			while (commonBlock && !isDescendant(commonBlock, gotoBlock)) {
				commonBlock = parent[commonBlock];
			}
		}

		if (labelBlock == commonBlock) continue;

		std::vector<Statement*>& targetBlock = commonBlock ? commonBlock->block : function.block;
		size_t insertIndex = targetBlock.size();

		for (size_t i = 0; i < targetBlock.size(); i++) {
			if (isDescendant(targetBlock[i], labelStatement)) {
				insertIndex = i;
				break;
			}
		}

		if (insertIndex == targetBlock.size()) continue;

		std::vector<Statement*>& oldBlock = labelBlock ? labelBlock->block : function.block;
		for (auto it = oldBlock.begin(); it != oldBlock.end(); ++it) {
			if (*it == labelStatement) {
				oldBlock.erase(it);
				break;
			}
		}

		targetBlock.insert(targetBlock.begin() + insertIndex, labelStatement);
		parent[labelStatement] = commonBlock;
	}

	// 第二遍：处理 goto 跳过局部变量声明的问题。
	// label 提升后，把位于 goto 与 label 之间的 DECLARATION 移到 goto 之前。
	parent.clear();
	labelStatements.clear();
	gotoStatements.clear();
	stack.clear();
	stack.push_back({ &function.block, nullptr });

	while (!stack.empty()) {
		const auto [block, owner] = stack.back();
		stack.pop_back();

		for (Statement* statement : *block) {
			if (!statement) continue;
			parent[statement] = owner;

			if (statement->type == AST_STATEMENT_LABEL) {
				const uint32_t label = statement->instruction.label;
				if (function.is_valid_label(label)) labelStatements[label] = statement;
			} else if (statement->type == AST_STATEMENT_GOTO) {
				const uint32_t label = statement->instruction.label;
				if (function.is_valid_label(label)) gotoStatements[label].push_back(statement);
			}

			if (!statement->block.empty()) stack.push_back({ &statement->block, statement });
		}
	}

	for (const auto& [label, labelStatement] : labelStatements) {
		const auto gotoIt = gotoStatements.find(label);
		if (gotoIt == gotoStatements.end() || gotoIt->second.empty()) continue;

		Statement* labelBlock = parent[labelStatement];
		std::vector<Statement*>& targetBlock = labelBlock ? labelBlock->block : function.block;
		size_t labelIndex = targetBlock.size();
		for (size_t i = 0; i < targetBlock.size(); i++) {
			if (targetBlock[i] == labelStatement) {
				labelIndex = i;
				break;
			}
		}
		if (labelIndex == targetBlock.size()) continue;

		size_t earliestGoto = labelIndex;
		for (Statement* gotoStatement : gotoIt->second) {
			for (size_t i = 0; i < targetBlock.size(); i++) {
				if (isDescendant(targetBlock[i], gotoStatement)) {
					if (i < earliestGoto) earliestGoto = i;
					break;
				}
			}
		}

		if (earliestGoto >= labelIndex) continue;

		std::vector<Statement*> declarations;
		for (size_t i = earliestGoto; i < labelIndex; i++) {
			if (targetBlock[i]->type == AST_STATEMENT_DECLARATION) declarations.push_back(targetBlock[i]);
		}
		if (declarations.empty()) continue;

		for (auto it = targetBlock.begin() + earliestGoto; it != targetBlock.begin() + labelIndex; ) {
			if ((*it)->type == AST_STATEMENT_DECLARATION) it = targetBlock.erase(it);
			else ++it;
		}
		targetBlock.insert(targetBlock.begin() + earliestGoto, declarations.begin(), declarations.end());
	}
}

bool Ast::expressions_equal(const Expression& a, const Expression& b) {
	if (a.type != b.type) return false;

	switch (a.type) {
	case AST_EXPRESSION_CONSTANT:
		if (a.constant->type != b.constant->type) return false;
		switch (a.constant->type) {
		case AST_CONSTANT_NIL:
		case AST_CONSTANT_FALSE:
		case AST_CONSTANT_TRUE:
			return true;
		case AST_CONSTANT_NUMBER:
			return a.constant->number == b.constant->number;
		case AST_CONSTANT_CDATA_SIGNED:
			return a.constant->signed_integer == b.constant->signed_integer;
		case AST_CONSTANT_CDATA_UNSIGNED:
		case AST_CONSTANT_CDATA_IMAGINARY:
			return a.constant->unsigned_integer == b.constant->unsigned_integer;
		case AST_CONSTANT_STRING:
			return a.constant->string == b.constant->string;
		}
		break;
	case AST_EXPRESSION_VARIABLE:
		if (a.variable->type != b.variable->type) return false;
		switch (a.variable->type) {
		case AST_VARIABLE_SLOT:
			return a.variable->slot == b.variable->slot;
		case AST_VARIABLE_UPVALUE:
			return a.variable->slotScope == b.variable->slotScope;
		case AST_VARIABLE_GLOBAL:
			return a.variable->name == b.variable->name;
		case AST_VARIABLE_TABLE_INDEX:
			return expressions_equal(*a.variable->table, *b.variable->table)
				&& expressions_equal(*a.variable->tableIndex, *b.variable->tableIndex);
		}
		break;
	case AST_EXPRESSION_UNARY_OPERATION:
		return a.unaryOperation->type == b.unaryOperation->type
			&& expressions_equal(*a.unaryOperation->operand, *b.unaryOperation->operand);
	case AST_EXPRESSION_BINARY_OPERATION:
		return a.binaryOperation->type == b.binaryOperation->type
			&& expressions_equal(*a.binaryOperation->leftOperand, *b.binaryOperation->leftOperand)
			&& expressions_equal(*a.binaryOperation->rightOperand, *b.binaryOperation->rightOperand);
	default:
		break;
	}

	return false;
}

bool Ast::expression_matches_variable(const Expression& expression, const Variable& variable) {
	if (expression.type != AST_EXPRESSION_VARIABLE) return false;

	switch (variable.type) {
	case AST_VARIABLE_SLOT:
		return expression.variable->type == AST_VARIABLE_SLOT && expression.variable->slot == variable.slot;
	case AST_VARIABLE_UPVALUE:
		return expression.variable->type == AST_VARIABLE_UPVALUE && expression.variable->slotScope == variable.slotScope;
	case AST_VARIABLE_GLOBAL:
		return expression.variable->type == AST_VARIABLE_GLOBAL && expression.variable->name == variable.name;
	case AST_VARIABLE_TABLE_INDEX:
		return expression.variable->type == AST_VARIABLE_TABLE_INDEX
			&& expressions_equal(*expression.variable->table, *variable.table)
			&& expressions_equal(*expression.variable->tableIndex, *variable.tableIndex);
	}

	return false;
}

bool Ast::variables_equal(const Variable& a, const Variable& b) {
	if (a.type != b.type) return false;

	switch (a.type) {
	case AST_VARIABLE_SLOT:
		return a.slot == b.slot;
	case AST_VARIABLE_UPVALUE:
		return a.slotScope == b.slotScope;
	case AST_VARIABLE_GLOBAL:
		return a.name == b.name;
	case AST_VARIABLE_TABLE_INDEX:
		return expressions_equal(*a.table, *b.table)
			&& expressions_equal(*a.tableIndex, *b.tableIndex);
	}

	return false;
}

bool Ast::is_boolean_expression(const Expression& expr) {
	switch (expr.type) {
	case AST_EXPRESSION_CONSTANT:
		return expr.constant->type == AST_CONSTANT_TRUE || expr.constant->type == AST_CONSTANT_FALSE;
	case AST_EXPRESSION_UNARY_OPERATION:
		return expr.unaryOperation->type == AST_UNARY_NOT;
	case AST_EXPRESSION_BINARY_OPERATION:
		switch (expr.binaryOperation->type) {
		case AST_BINARY_LESS_THAN:
		case AST_BINARY_LESS_EQUAL:
		case AST_BINARY_GREATER_THEN:
		case AST_BINARY_GREATER_EQUAL:
		case AST_BINARY_EQUAL:
		case AST_BINARY_NOT_EQUAL:
			return true;
		case AST_BINARY_AND:
		case AST_BINARY_OR:
			return is_boolean_expression(*expr.binaryOperation->leftOperand)
				&& is_boolean_expression(*expr.binaryOperation->rightOperand);
		default:
			return false;
		}
	default:
		return false;
	}
}

Ast::Expression* Ast::new_unary_operation(const AST_UNARY_OPERATION& type, Expression* const& operand) {
	Expression* const expression = new_expression(AST_EXPRESSION_UNARY_OPERATION);
	expression->unaryOperation->type = type;
	expression->unaryOperation->operand = operand;
	return expression;
}

Ast::Expression* Ast::new_binary_operation(const AST_BINARY_OPERATION& type, Expression* const& left, Expression* const& right) {
	Expression* const expression = new_expression(AST_EXPRESSION_BINARY_OPERATION);
	expression->binaryOperation->type = type;
	expression->binaryOperation->leftOperand = left;
	expression->binaryOperation->rightOperand = right;
	return expression;
}

Ast::Expression* Ast::invert_expression(Expression* const& expr) {
	if (!expr) return nullptr;

	if (expr->type == AST_EXPRESSION_UNARY_OPERATION && expr->unaryOperation->type == AST_UNARY_NOT) {
		if (is_boolean_expression(*expr->unaryOperation->operand)) {
			return expr->unaryOperation->operand;
		}
	}

	if (expr->type == AST_EXPRESSION_BINARY_OPERATION) {
		AST_BINARY_OPERATION invertedType;
		bool canInvert = true;
		switch (expr->binaryOperation->type) {
		case AST_BINARY_LESS_THAN:
			invertedType = AST_BINARY_GREATER_EQUAL;
			break;
		case AST_BINARY_LESS_EQUAL:
			invertedType = AST_BINARY_GREATER_THEN;
			break;
		case AST_BINARY_GREATER_THEN:
			invertedType = AST_BINARY_LESS_EQUAL;
			break;
		case AST_BINARY_GREATER_EQUAL:
			invertedType = AST_BINARY_LESS_THAN;
			break;
		case AST_BINARY_EQUAL:
			invertedType = AST_BINARY_NOT_EQUAL;
			break;
		case AST_BINARY_NOT_EQUAL:
			invertedType = AST_BINARY_EQUAL;
			break;
		default:
			canInvert = false;
			break;
		}

		if (canInvert) {
			return new_binary_operation(invertedType, expr->binaryOperation->leftOperand, expr->binaryOperation->rightOperand);
		}
	}

	if (expr->type == AST_EXPRESSION_CONSTANT) {
		if (expr->constant->type == AST_CONSTANT_TRUE) return new_primitive(1); // false
		if (expr->constant->type == AST_CONSTANT_FALSE) return new_primitive(2); // true
	}

	return new_unary_operation(AST_UNARY_NOT, expr);
}

Ast::Expression* Ast::build_boolean_test(Expression* const& expr, const bool& isTruthy) {
	if (isTruthy) {
		if (is_boolean_expression(*expr)) return expr;
		return new_unary_operation(AST_UNARY_NOT, new_unary_operation(AST_UNARY_NOT, expr));
	}
	return invert_expression(expr);
}

Ast::Expression* Ast::simplify_expression(Expression* const& expr) {
	if (!expr) return nullptr;

	if (expr->type == AST_EXPRESSION_UNARY_OPERATION) {
		expr->unaryOperation->operand = simplify_expression(expr->unaryOperation->operand);
		if (expr->unaryOperation->type == AST_UNARY_NOT) {
			Expression* inner = expr->unaryOperation->operand;
			if (inner->type == AST_EXPRESSION_BINARY_OPERATION) {
				switch (inner->binaryOperation->type) {
				case AST_BINARY_LESS_THAN:
				case AST_BINARY_LESS_EQUAL:
				case AST_BINARY_GREATER_THEN:
				case AST_BINARY_GREATER_EQUAL:
				case AST_BINARY_EQUAL:
				case AST_BINARY_NOT_EQUAL:
					return invert_expression(inner);
				default:
					break;
				}
			}
			if (inner->type == AST_EXPRESSION_UNARY_OPERATION && inner->unaryOperation->type == AST_UNARY_NOT) {
				if (is_boolean_expression(*inner->unaryOperation->operand)) {
					return inner->unaryOperation->operand;
				}
			}
		}
		return expr;
	}

	if (expr->type == AST_EXPRESSION_BINARY_OPERATION) {
		expr->binaryOperation->leftOperand = simplify_expression(expr->binaryOperation->leftOperand);
		expr->binaryOperation->rightOperand = simplify_expression(expr->binaryOperation->rightOperand);

		// Helper to extract condition from an AND chain that terminates with a specific constant
		auto extract_and_const_condition = [&](const auto& self, Expression* e, const AST_CONSTANT& constType)->Expression* {
			if (!e || e->type != AST_EXPRESSION_BINARY_OPERATION || e->binaryOperation->type != AST_BINARY_AND) return nullptr;
			if (e->binaryOperation->rightOperand->type == AST_EXPRESSION_CONSTANT
				&& e->binaryOperation->rightOperand->constant->type == constType) {
				return e->binaryOperation->leftOperand;
			}
			Expression* sub = self(self, e->binaryOperation->rightOperand, constType);
			if (sub) {
				return new_binary_operation(AST_BINARY_AND, e->binaryOperation->leftOperand, sub);
			}
			return nullptr;
		};

		// (cond and true) or false  =>  cond / not not cond
		// (cond and false) or true  =>  invert(cond)
		if (expr->binaryOperation->type == AST_BINARY_OR) {
			Expression* left = expr->binaryOperation->leftOperand;
			Expression* right = expr->binaryOperation->rightOperand;

			if (right->type == AST_EXPRESSION_CONSTANT && right->constant->type == AST_CONSTANT_FALSE) {
				Expression* cond = extract_and_const_condition(extract_and_const_condition, left, AST_CONSTANT_TRUE);
				if (cond) return build_boolean_test(cond, true);
			} else if (right->type == AST_EXPRESSION_CONSTANT && right->constant->type == AST_CONSTANT_TRUE) {
				Expression* cond = extract_and_const_condition(extract_and_const_condition, left, AST_CONSTANT_FALSE);
				if (cond) return build_boolean_test(cond, false);
			}
		}

		if (expr->binaryOperation->type == AST_BINARY_AND) {
			Expression* left = expr->binaryOperation->leftOperand;
			Expression* right = expr->binaryOperation->rightOperand;

			if (right->type == AST_EXPRESSION_CONSTANT && right->constant->type == AST_CONSTANT_TRUE) {
				if (left->type == AST_EXPRESSION_BINARY_OPERATION && left->binaryOperation->type == AST_BINARY_OR) {
					if (left->binaryOperation->rightOperand->type == AST_EXPRESSION_CONSTANT
						&& left->binaryOperation->rightOperand->constant->type == AST_CONSTANT_FALSE) {
						return build_boolean_test(left->binaryOperation->leftOperand, true);
					}
				}
			}
		}

		return expr;
	}

	return expr;
}

void Ast::optimize_conditional_assignments(Function& function) {
	optimize_conditional_assignments(function, function.block);
}

void Ast::optimize_conditional_assignments(Function& function, std::vector<Statement*>& block) {
	for (Statement* statement : block) {
		if (statement && !statement->block.empty()) optimize_conditional_assignments(function, statement->block);
	}

	// 1. 简化所有已有赋值中的表达式 (如 (a > b) and true or false -> a > b)
	for (Statement* statement : block) {
		if (!statement) continue;
		for (Expression*& expr : statement->assignment.expressions) {
			expr = simplify_expression(expr);
		}
	}

	// 2. 遍历合并条件赋值模式
	for (uint32_t i = 0; i < block.size(); i++) {
		// Pattern A: if-else 对同一变量赋值
		if (i + 1 < block.size()) {
			Statement* ifStatement = block[i];
			Statement* elseStatement = block[i + 1];

			if (ifStatement && elseStatement
				&& ifStatement->type == AST_STATEMENT_IF
				&& elseStatement->type == AST_STATEMENT_ELSE
				&& ifStatement->assignment.expressions.size() == 1
				&& ifStatement->block.size() == 1
				&& elseStatement->block.size() == 1) {
				Statement* trueAssign = ifStatement->block.front();
				Statement* falseAssign = elseStatement->block.front();

				if (trueAssign && falseAssign
					&& trueAssign->type == AST_STATEMENT_ASSIGNMENT
					&& falseAssign->type == AST_STATEMENT_ASSIGNMENT
					&& trueAssign->assignment.variables.size() == 1
					&& falseAssign->assignment.variables.size() == 1
					&& trueAssign->assignment.expressions.size() == 1
					&& falseAssign->assignment.expressions.size() == 1
					&& variables_equal(trueAssign->assignment.variables.front(), falseAssign->assignment.variables.front())) {

					Expression* cond = ifStatement->assignment.expressions.front();
					Expression* trueVal = trueAssign->assignment.expressions.front();
					Expression* falseVal = falseAssign->assignment.expressions.front();

					bool v1IsTrue = (trueVal->type == AST_EXPRESSION_CONSTANT && trueVal->constant->type == AST_CONSTANT_TRUE);
					bool v1IsFalse = (trueVal->type == AST_EXPRESSION_CONSTANT && trueVal->constant->type == AST_CONSTANT_FALSE);
					bool v2IsTrue = (falseVal->type == AST_EXPRESSION_CONSTANT && falseVal->constant->type == AST_CONSTANT_TRUE);
					bool v2IsFalse = (falseVal->type == AST_EXPRESSION_CONSTANT && falseVal->constant->type == AST_CONSTANT_FALSE);

					Expression* resultExpr = nullptr;
					if (v1IsTrue && v2IsFalse) {
						resultExpr = build_boolean_test(cond, true);
					} else if (v1IsFalse && v2IsTrue) {
						resultExpr = build_boolean_test(cond, false);
					} else if (!v1IsFalse && !(trueVal->type == AST_EXPRESSION_CONSTANT && trueVal->constant->type == AST_CONSTANT_NIL)) {
						resultExpr = new_binary_operation(AST_BINARY_OR,
							new_binary_operation(AST_BINARY_AND, cond, trueVal),
							falseVal);
					}

					if (resultExpr) {
						resultExpr = simplify_expression(resultExpr);

						// 若前面紧邻该变量的空声明，直接合并为 `local x = resultExpr`
						if (i > 0
							&& block[i - 1]->type == AST_STATEMENT_DECLARATION
							&& block[i - 1]->assignment.variables.size() == 1
							&& block[i - 1]->assignment.expressions.empty()
							&& variables_equal(block[i - 1]->assignment.variables.front(), trueAssign->assignment.variables.front())) {
							block[i - 1]->assignment.expressions.push_back(resultExpr);
							block.erase(block.begin() + i, block.begin() + i + 2);
							i = i > 1 ? i - 2 : 0;
							continue;
						}

						trueAssign->assignment.expressions.front() = resultExpr;
						trueAssign->instruction = ifStatement->instruction;
						block[i] = trueAssign;
						block.erase(block.begin() + i + 1);
						i = i ? i - 1 : 0;
						continue;
					}
				}
			}
		}

		// Pattern B: declaration/assignment 紧接 if not a then a = default end
		if (i + 1 < block.size()) {
			Statement* prev = block[i];
			Statement* ifStatement = block[i + 1];

			if (prev && ifStatement
				&& (prev->type == AST_STATEMENT_DECLARATION || prev->type == AST_STATEMENT_ASSIGNMENT)
				&& ifStatement->type == AST_STATEMENT_IF
				&& (i + 2 >= block.size() || block[i + 2]->type != AST_STATEMENT_ELSE)
				&& prev->assignment.variables.size() == 1
				&& ifStatement->assignment.expressions.size() == 1
				&& ifStatement->assignment.expressions.back()->type == AST_EXPRESSION_UNARY_OPERATION
				&& ifStatement->assignment.expressions.back()->unaryOperation->type == AST_UNARY_NOT
				&& ifStatement->block.size() == 1) {

				Expression* conditionOperand = ifStatement->assignment.expressions.back()->unaryOperation->operand;
				Statement* innerAssignment = ifStatement->block.front();

				if (innerAssignment
					&& innerAssignment->type == AST_STATEMENT_ASSIGNMENT
					&& innerAssignment->assignment.variables.size() == 1
					&& innerAssignment->assignment.expressions.size() == 1
					&& variables_equal(innerAssignment->assignment.variables.front(), prev->assignment.variables.front())
					&& (prev->assignment.expressions.empty() || expression_matches_variable(*conditionOperand, prev->assignment.variables.front()))) {

					Expression* defaultValue = innerAssignment->assignment.expressions.back();
					Expression* baseValue = prev->assignment.expressions.empty()
						? conditionOperand
						: prev->assignment.expressions.back();

					Expression* orExpression = new_binary_operation(AST_BINARY_OR, baseValue, defaultValue);
					orExpression = simplify_expression(orExpression);

					prev->assignment.expressions.clear();
					prev->assignment.expressions.emplace_back(orExpression);
					block.erase(block.begin() + i + 1);

					// 若变量紧随其后被直接赋值给同一目标，则进一步内联为 `target = target or default`
					if (i + 1 < block.size()) {
						Statement* assignment = block[i + 1];
						if (assignment
							&& assignment->type == AST_STATEMENT_ASSIGNMENT
							&& assignment->assignment.variables.size() == 1
							&& assignment->assignment.expressions.size() == 1
							&& expression_matches_variable(*assignment->assignment.expressions.back(), prev->assignment.variables.back())
							&& expression_matches_variable(*conditionOperand, assignment->assignment.variables.back())) {
							assignment->assignment.expressions.back() = orExpression;
							block.erase(block.begin() + i);
						}
					}

					i = i ? i - 1 : 0;
					continue;
				}
			}
		}

		// Pattern C: 独立的 `if not a then a = default end` (作为普通变量默认赋值)
		if (block[i]->type == AST_STATEMENT_IF
			&& (i + 1 >= block.size() || block[i + 1]->type != AST_STATEMENT_ELSE)
			&& block[i]->assignment.expressions.size() == 1
			&& block[i]->assignment.expressions.back()->type == AST_EXPRESSION_UNARY_OPERATION
			&& block[i]->assignment.expressions.back()->unaryOperation->type == AST_UNARY_NOT
			&& block[i]->block.size() == 1) {

			Expression* conditionOperand = block[i]->assignment.expressions.back()->unaryOperation->operand;
			Statement* innerAssignment = block[i]->block.front();

			if (innerAssignment
				&& innerAssignment->type == AST_STATEMENT_ASSIGNMENT
				&& innerAssignment->assignment.variables.size() == 1
				&& innerAssignment->assignment.expressions.size() == 1
				&& expression_matches_variable(*conditionOperand, innerAssignment->assignment.variables.front())) {

				Expression* orExpression = new_binary_operation(AST_BINARY_OR, conditionOperand, innerAssignment->assignment.expressions.back());
				orExpression = simplify_expression(orExpression);

				innerAssignment->assignment.expressions.back() = orExpression;
				innerAssignment->instruction = block[i]->instruction;
				block[i] = innerAssignment;
				i = i ? i - 1 : 0;
				continue;
			}
		}

		// Pattern D: 紧邻的空声明与赋值/声明合并 (local x; x = expr => local x = expr)
		if (i + 1 < block.size()) {
			Statement* decl = block[i];
			Statement* nextStmt = block[i + 1];

			if (decl && nextStmt
				&& decl->type == AST_STATEMENT_DECLARATION
				&& (nextStmt->type == AST_STATEMENT_ASSIGNMENT || nextStmt->type == AST_STATEMENT_DECLARATION)
				&& decl->assignment.expressions.empty()
				&& decl->assignment.variables.size() == 1
				&& nextStmt->assignment.variables.size() == 1
				&& nextStmt->assignment.expressions.size() == 1
				&& variables_equal(decl->assignment.variables.front(), nextStmt->assignment.variables.front())) {

				bool selfRef = false;
				if (decl->assignment.variables.front().type == AST_VARIABLE_SLOT) {
					selfRef = has_self_reference(decl->assignment.variables.front().slot, nextStmt->assignment.expressions.front());
				}

				if (!selfRef) {
					if (decl->assignment.variables.front().type == AST_VARIABLE_SLOT
						&& nextStmt->assignment.variables.front().type == AST_VARIABLE_SLOT) {
						(*nextStmt->assignment.variables.front().slotScope)->name = (*decl->assignment.variables.front().slotScope)->name;
					}
					decl->assignment.expressions.push_back(nextStmt->assignment.expressions.front());
					block.erase(block.begin() + i + 1);
					i = i ? i - 1 : 0;
					continue;
				}
			}
		}
	}
}

void Ast::cleanup_unused_declarations(Function& function, std::vector<Statement*>& block) {
	for (Statement* statement : block) {
		if (statement && !statement->block.empty()) cleanup_unused_declarations(function, statement->block);
	}

	// 子函数 (闭包) 通过 upvalue 捕获的槽位不在此函数树内, 删除声明会让闭包
	// 引用悬空, 必须保留。
	std::unordered_set<const SlotScope*> capturedScopes;
	collect_captured_scopes(function, capturedScopes);

	// 收集在所有表达式和非空赋值中被引用的作用域 (按作用域身份, 而非槽位号)
	std::unordered_set<const SlotScope*> usedScopes;

	auto collect_from_expr = [&](const auto& self, Expression* const& expr)->void {
		if (!expr) return;
		switch (expr->type) {
		case AST_EXPRESSION_VARIABLE:
			if (expr->variable->type == AST_VARIABLE_SLOT) {
				if (expr->variable->slotScope && *expr->variable->slotScope) {
					usedScopes.insert(*expr->variable->slotScope);
				}
			} else if (expr->variable->type == AST_VARIABLE_TABLE_INDEX) {
				self(self, expr->variable->table);
				self(self, expr->variable->tableIndex);
			}
			break;
		case AST_EXPRESSION_TABLE:
			for (const auto& f : expr->table->fields) {
				self(self, f.key);
				self(self, f.value);
			}
			break;
		case AST_EXPRESSION_FUNCTION_CALL:
			for (Expression* arg : expr->functionCall->arguments) self(self, arg);
			break;
		case AST_EXPRESSION_UNARY_OPERATION:
			self(self, expr->unaryOperation->operand);
			break;
		case AST_EXPRESSION_BINARY_OPERATION:
			self(self, expr->binaryOperation->leftOperand);
			self(self, expr->binaryOperation->rightOperand);
			break;
		default:
			break;
		}
	};

	auto collect_from_statement = [&](const auto& self, Statement* const& stmt)->void {
		if (!stmt) return;
		for (Expression* expr : stmt->assignment.expressions) collect_from_expr(collect_from_expr, expr);
		if (stmt->assignment.multresReturn) collect_from_expr(collect_from_expr, stmt->assignment.multresReturn);
		if (stmt->type != AST_STATEMENT_DECLARATION) {
			for (const Variable& var : stmt->assignment.variables) {
				if (var.type == AST_VARIABLE_SLOT) {
					if (var.slotScope && *var.slotScope) usedScopes.insert(*var.slotScope);
				}
				else if (var.type == AST_VARIABLE_TABLE_INDEX) {
					collect_from_expr(collect_from_expr, var.table);
					collect_from_expr(collect_from_expr, var.tableIndex);
				}
			}
		}
		for (Statement* child : stmt->block) self(self, child);
	};

	for (Statement* s : function.block) collect_from_statement(collect_from_statement, s);

	// 移除从未被引用的声明: 无初始值的空声明, 或纯初始值且无副作用的单变量声明。
	for (auto it = block.begin(); it != block.end();) {
		Statement* stmt = *it;
		if (stmt && stmt->type == AST_STATEMENT_DECLARATION
			&& !function.is_valid_label(stmt->instruction.label)
			&& stmt->assignment.variables.size() == 1
			&& stmt->assignment.variables.back().type == AST_VARIABLE_SLOT
			&& stmt->assignment.variables.back().slotScope
			&& *stmt->assignment.variables.back().slotScope) {
			const SlotScope* scope = *stmt->assignment.variables.back().slotScope;
			const bool isUnused = !usedScopes.contains(scope) && !capturedScopes.contains(scope);
			if (isUnused) {
				const bool empty = stmt->assignment.expressions.empty();
				const bool pureInit = !empty
					&& stmt->assignment.expressions.size() == 1
					&& stmt->assignment.expressions.back()
					&& !expression_has_side_effects(stmt->assignment.expressions.back())
					&& !expression_references_scope(stmt->assignment.expressions.back(), scope);
				// 空声明沿用旧行为: 仅剥离调试信息的字节码才删除 (避免改动真实局部变量输出);
				// 纯初始化的死声明对任何字节码都可安全删除。
				if ((empty && !function.hasDebugInfo) || pureInit) {
					it = block.erase(it);
					continue;
				}
			}
		}
		++it;
	}
}

void Ast::fix_out_of_scope_declarations(Function& function) {
	std::unordered_map<Statement*, Statement*> parent;
	std::vector<std::pair<std::vector<Statement*>*, Statement*>> stack;
	stack.push_back({ &function.block, nullptr });

	while (!stack.empty()) {
		const auto [block, owner] = stack.back();
		stack.pop_back();
		for (Statement* statement : *block) {
			if (!statement) continue;
			parent[statement] = owner;
			if (!statement->block.empty()) stack.push_back({ &statement->block, statement });
		}
	}

	std::vector<Statement*> declarations;
	for (const auto& [statement, owner] : parent) {
		if (!statement || statement->type != AST_STATEMENT_DECLARATION) continue;
		if (statement->assignment.variables.size() != 1
			|| statement->assignment.variables.back().type != AST_VARIABLE_SLOT)
			continue;
		declarations.push_back(statement);
	}

	const auto isDescendant = [&](Statement* ancestor, Statement* target) -> bool {
		for (Statement* current = target; current; current = parent[current]) {
			if (current == ancestor) return true;
		}
		return false;
	};

	for (Statement* declaration : declarations) {
		Statement* innerOwner = parent[declaration];
		if (!innerOwner) continue;

		const uint8_t slot = declaration->assignment.variables.back().slot;
		SlotScope* slotScope = *declaration->assignment.variables.back().slotScope;
		bool usedOutside = false;

		const auto scanExpression = [&](auto&& self, Expression*& expression, Statement* statement) -> void {
			if (!expression || usedOutside) return;
			switch (expression->type) {
			case AST_EXPRESSION_VARIABLE:
				if (expression->variable->type == AST_VARIABLE_SLOT
					&& expression->variable->slot == slot
					&& expression->variable->slotScope
					&& *expression->variable->slotScope == slotScope
					&& !isDescendant(innerOwner, statement)) {
					usedOutside = true;
					return;
				}
				if (expression->variable->table) self(self, expression->variable->table, statement);
				if (expression->variable->tableIndex) self(self, expression->variable->tableIndex, statement);
				break;
			case AST_EXPRESSION_FUNCTION_CALL:
				self(self, expression->functionCall->function, statement);
				for (Expression*& argument : expression->functionCall->arguments) self(self, argument, statement);
				if (expression->functionCall->multresArgument) self(self, expression->functionCall->multresArgument, statement);
				break;
			case AST_EXPRESSION_TABLE:
				for (auto& field : expression->table->fields) {
					self(self, field.key, statement);
					self(self, field.value, statement);
				}
				if (expression->table->multresField) self(self, expression->table->multresField, statement);
				break;
			case AST_EXPRESSION_BINARY_OPERATION:
				self(self, expression->binaryOperation->leftOperand, statement);
				self(self, expression->binaryOperation->rightOperand, statement);
				break;
			case AST_EXPRESSION_UNARY_OPERATION:
				self(self, expression->unaryOperation->operand, statement);
				break;
			default:
				break;
			}
		};

		const auto scanStatement = [&](auto&& self, Statement* statement) -> void {
			if (!statement || usedOutside) return;
			for (auto& variable : statement->assignment.variables) {
				if (variable.table) scanExpression(scanExpression, variable.table, statement);
				if (variable.tableIndex) scanExpression(scanExpression, variable.tableIndex, statement);
			}
			for (Expression*& expression : statement->assignment.expressions) {
				scanExpression(scanExpression, expression, statement);
			}
			if (statement->assignment.multresReturn) scanExpression(scanExpression, statement->assignment.multresReturn, statement);
			for (Statement* child : statement->block) self(self, child);
		};

		for (Statement* statement : function.block) scanStatement(scanStatement, statement);
		if (!usedOutside) continue;

		Statement* outerOwner = parent[innerOwner];
		std::vector<Statement*>& outerBlock = outerOwner ? outerOwner->block : function.block;
		size_t insertIndex = outerBlock.size();
		for (size_t i = 0; i < outerBlock.size(); i++) {
			if (outerBlock[i] == innerOwner) {
				insertIndex = i;
				break;
			}
		}
		if (insertIndex == outerBlock.size()) continue;

		Statement* outerDeclaration = new_statement(AST_STATEMENT_DECLARATION);
		outerDeclaration->assignment.variables.push_back(declaration->assignment.variables.back());
		outerBlock.insert(outerBlock.begin() + insertIndex, outerDeclaration);

		if (declaration->assignment.expressions.size() == 1) {
			declaration->type = AST_STATEMENT_ASSIGNMENT;
		} else {
			std::vector<Statement*>& innerBlock = innerOwner->block;
			for (auto it = innerBlock.begin(); it != innerBlock.end(); ++it) {
				if (*it == declaration) {
					innerBlock.erase(it);
					break;
				}
			}
		}
	}
}

void Ast::clean_up_block(Function& function, std::vector<Statement*>& block, uint32_t& variableCounter, uint32_t& iteratorCounter, BlockInfo* const& previousBlock) {
	//TODO
	BlockInfo blockInfo = { .block = block, .previousBlock = previousBlock };
	std::vector<Variable*> declarations;
	Statement** declarationTarget;

	for (uint32_t i = 0; i < block.size(); i++) {
		switch (block[i]->type) {
		case AST_STATEMENT_NUMERIC_FOR:
			if (block[i]->assignment.expressions.back()->type == AST_EXPRESSION_CONSTANT
				&& block[i]->assignment.expressions.back()->constant->type == AST_CONSTANT_NUMBER
				&& block[i]->assignment.expressions.back()->constant->number == 1)
				block[i]->assignment.expressions.pop_back();
		case AST_STATEMENT_GENERIC_FOR:
			if (block[i]->type == AST_STATEMENT_GENERIC_FOR) {
				while (block[i]->assignment.expressions.size() > 1
					&& block[i]->assignment.expressions.back()->type == AST_EXPRESSION_CONSTANT
					&& block[i]->assignment.expressions.back()->constant->type == AST_CONSTANT_NIL) {
					block[i]->assignment.expressions.pop_back();
				}
			}

			if (function.hasDebugInfo) {
				for (uint8_t j = block[i]->assignment.variables.size(); j--;) {
					(*block[i]->assignment.variables[j].slotScope)->name = block[i]->locals->names[j];
				}
			} else {
				for (uint8_t j = 0; j < block[i]->assignment.variables.size(); j++) {
					(*block[i]->assignment.variables[j].slotScope)->name = "iter_" + std::to_string(minimizeDiffs ? function.level : function.id) + "_" + std::to_string(iteratorCounter);
					iteratorCounter++;
				}
			}

			clean_up_block(function, block[i]->block, variableCounter, iteratorCounter, nullptr);
			continue;
		case AST_STATEMENT_LOOP:
			for (std::vector<Statement*>* currentBlock = &block[i]->block; currentBlock->size(); currentBlock = &currentBlock->back()->block) {
				if (currentBlock->back()->type == AST_STATEMENT_DECLARATION) continue;
				if (currentBlock->back()->type != AST_STATEMENT_GOTO
					|| currentBlock->back()->instruction.target != block[i]->instruction.id
					|| function.is_valid_label(currentBlock->back()->instruction.label)
					|| currentBlock->size() < 2)
					break;

				switch ((*currentBlock)[currentBlock->size() - 2]->type) {
				case AST_STATEMENT_BREAK:
					function.remove_jump(currentBlock->back()->instruction.id, currentBlock->back()->instruction.target);
					function.remove_jump((*currentBlock)[currentBlock->size() - 2]->instruction.id, (*currentBlock)[currentBlock->size() - 2]->instruction.target);
					block[i]->type = AST_STATEMENT_REPEAT;
					block[i]->assignment.expressions.emplace_back(new_primitive(2));
					(*currentBlock)[currentBlock->size() - 2]->type = AST_STATEMENT_EMPTY;
					currentBlock->erase(currentBlock->begin() + currentBlock->size() - 1);
					break;
				case AST_STATEMENT_IF:
					if ((*currentBlock)[currentBlock->size() - 2]->block.size() != 1 || (*currentBlock)[currentBlock->size() - 2]->block.back()->type != AST_STATEMENT_BREAK) break;
					function.remove_jump(currentBlock->back()->instruction.id, currentBlock->back()->instruction.target);
					function.remove_jump((*currentBlock)[currentBlock->size() - 2]->block.back()->instruction.id, (*currentBlock)[currentBlock->size() - 2]->block.back()->instruction.target);
					block[i]->type = AST_STATEMENT_REPEAT;
					block[i]->assignment.expressions.emplace_back((*currentBlock)[currentBlock->size() - 2]->assignment.expressions.back());
					(*currentBlock)[currentBlock->size() - 2]->type = AST_STATEMENT_EMPTY;
					currentBlock->erase(currentBlock->begin() + currentBlock->size() - 1);
					break;
				}

				break;
			}

			if (block[i]->type == AST_STATEMENT_LOOP) {
				if (block[i]->block.size() && block[i]->block.back()->type == AST_STATEMENT_GOTO) {
					if (block[i]->instruction.id == block[i]->block.back()->instruction.target) {
						function.remove_jump(block[i]->block.back()->instruction.id, block[i]->block.back()->instruction.target);
						block[i]->type = AST_STATEMENT_WHILE;
						block[i]->assignment.expressions.emplace_back(new_primitive(2));
						block[i]->block.back()->type = AST_STATEMENT_EMPTY;
					} else if (block.size() != 1) {
						for (uint32_t j = i; j--
							&& block[j]->type == AST_STATEMENT_IF
							&& !block[j]->block.size();) {
							if (!function.is_valid_label(block[j]->instruction.label)) continue;
							if (function.is_valid_label(block[i]->instruction.label) || function.labels[block[j]->instruction.label].target != block[i]->block.back()->instruction.target) break;
							function.remove_jump(block[i]->block.back()->instruction.id, block[i]->block.back()->instruction.target);
							block[i]->type = AST_STATEMENT_WHILE;
							block[i]->instruction.label = block[j]->instruction.label;

							for (Expression* expression; i != j; i--) {
								expression = new_expression(AST_EXPRESSION_BINARY_OPERATION);
								expression->binaryOperation->type = AST_BINARY_OR;
								expression->binaryOperation->rightOperand = new_primitive(2);
								expression->binaryOperation->leftOperand = block[i - 1]->assignment.expressions.back();

								if (block[i]->assignment.expressions.size()) {
									block[i - 1]->assignment.expressions.back() = new_expression(AST_EXPRESSION_BINARY_OPERATION);
									block[i - 1]->assignment.expressions.back()->binaryOperation->type = AST_BINARY_AND;
									block[i - 1]->assignment.expressions.back()->binaryOperation->rightOperand = block[i]->assignment.expressions.back();
									block[i - 1]->assignment.expressions.back()->binaryOperation->leftOperand = expression;
									block[i]->assignment.expressions.back() = block[i - 1]->assignment.expressions.back();
								} else {
									block[i]->assignment.expressions.resize(1, expression);
								}

								block.erase(block.begin() + i - 1);
							}

							block[i]->block.back()->type = AST_STATEMENT_EMPTY;
							break;
						}
					} else if (previousBlock
						&& previousBlock->block[previousBlock->index]->type == AST_STATEMENT_IF
						&& !function.is_valid_label(block[i]->instruction.label)) {
						if (function.is_valid_label(previousBlock->block[previousBlock->index]->instruction.label)) {
							if (function.labels[previousBlock->block[previousBlock->index]->instruction.label].target == block[i]->block.back()->instruction.target) {
								function.remove_jump(block[i]->block.back()->instruction.id, block[i]->block.back()->instruction.target);
								block[i]->type = AST_STATEMENT_WHILE;
								block[i]->assignment.expressions.emplace_back(previousBlock->block[previousBlock->index]->assignment.expressions.back());
								block[i]->instruction.label = previousBlock->block[previousBlock->index]->instruction.label;
								block[i]->block.back()->type = AST_STATEMENT_EMPTY;
								previousBlock->block[previousBlock->index] = block[i];
							}
						} else {
							for (uint32_t j = previousBlock->index - 1; j--
								&& previousBlock->block[j]->type == AST_STATEMENT_IF
								&& !previousBlock->block[j]->block.size();) {
								if (!function.is_valid_label(previousBlock->block[j]->instruction.label)) continue;
								if (function.labels[previousBlock->block[j]->instruction.label].target != block[i]->block.back()->instruction.target) break;
								function.remove_jump(block[i]->block.back()->instruction.id, block[i]->block.back()->instruction.target);
								block[i]->type = AST_STATEMENT_WHILE;
								block[i]->assignment.expressions.resize(1, new_expression(AST_EXPRESSION_BINARY_OPERATION));
								block[i]->assignment.expressions.back()->binaryOperation->type = AST_BINARY_AND;
								block[i]->assignment.expressions.back()->binaryOperation->rightOperand = previousBlock->block[previousBlock->index]->assignment.expressions.back();
								block[i]->instruction.label = previousBlock->block[j]->instruction.label;
								previousBlock->block[j]->instruction.label = INVALID_ID;

								for (Expression* expression; j != previousBlock->index - 1; j++) {
									expression = new_expression(AST_EXPRESSION_BINARY_OPERATION);
									expression->binaryOperation->type = AST_BINARY_OR;
									expression->binaryOperation->rightOperand = new_primitive(2);
									expression->binaryOperation->leftOperand = previousBlock->block[j]->assignment.expressions.back();

									if (block[i]->assignment.expressions.back()->binaryOperation->leftOperand) {
										previousBlock->block[j]->assignment.expressions.back() = new_expression(AST_EXPRESSION_BINARY_OPERATION);
										previousBlock->block[j]->assignment.expressions.back()->binaryOperation->type = AST_BINARY_AND;
										previousBlock->block[j]->assignment.expressions.back()->binaryOperation->rightOperand = expression;
										previousBlock->block[j]->assignment.expressions.back()->binaryOperation->leftOperand = block[i]->assignment.expressions.back()->binaryOperation->leftOperand;
										block[i]->assignment.expressions.back()->binaryOperation->leftOperand = previousBlock->block[j]->assignment.expressions.back();
									} else {
										block[i]->assignment.expressions.back()->binaryOperation->leftOperand = expression;
									}

									previousBlock->block[j]->type = AST_STATEMENT_EMPTY;
								}

								block[i]->block.back()->type = AST_STATEMENT_EMPTY;
								previousBlock->block[previousBlock->index] = block[i];
								break;
							}
						}
					}
				}

				if (block[i]->type == AST_STATEMENT_LOOP) {
					block[i]->type = AST_STATEMENT_REPEAT;
					block[i]->assignment.expressions.emplace_back(new_primitive(2));
				}
			}

			clean_up_block(function, block[i]->block, variableCounter, iteratorCounter, nullptr);
			continue;
		case AST_STATEMENT_BREAK:
			function.remove_jump(block[i]->instruction.id, block[i]->instruction.target);
			continue;
		case AST_STATEMENT_DECLARATION:
			while (block[i]->assignment.expressions.size()
				&& block[i]->assignment.expressions.back()->type == AST_EXPRESSION_CONSTANT
				&& block[i]->assignment.expressions.back()->constant->type == AST_CONSTANT_NIL) {
				block[i]->assignment.expressions.pop_back();
			}

			for (uint8_t j = block[i]->assignment.variables.size(); j--;) {
				(*block[i]->assignment.variables[j].slotScope)->name = block[i]->locals->names[j];
			}

			clean_up_block(function, block[i]->block, variableCounter, iteratorCounter, nullptr);
			continue;
		case AST_STATEMENT_ASSIGNMENT:
			if (block[i]->assignment.variables.size() == 1
				&& block[i]->assignment.variables.back().type == AST_VARIABLE_SLOT
				&& !(*block[i]->assignment.variables.back().slotScope)->usages
				&& block[i]->assignment.expressions.size() == 1
				&& block[i]->assignment.expressions.back()->type == AST_EXPRESSION_TABLE
				&& block[i]->assignment.expressions.back()->table->fields.size() == 1
				&& !block[i]->assignment.expressions.back()->table->constants.list.size()
				&& !block[i]->assignment.expressions.back()->table->constants.fields.size()
				&& !block[i]->assignment.expressions.back()->table->multresField) {
				function.slotScopeCollector.remove_scope(block[i]->assignment.variables.back().slot, block[i]->assignment.variables.back().slotScope);
				block[i]->assignment.variables.back().type = AST_VARIABLE_TABLE_INDEX;
				block[i]->assignment.variables.back().table = block[i]->assignment.expressions.back();
				block[i]->assignment.variables.back().tableIndex = block[i]->assignment.expressions.back()->table->fields.back().key;
				block[i]->assignment.expressions.back() = block[i]->assignment.expressions.back()->table->fields.back().value;
				block[i]->assignment.variables.back().table->table->fields.pop_back();
				continue;
			}

			for (uint32_t j = 0; j < block[i]->assignment.variables.size(); j++) {
				if (block[i]->assignment.variables[j].type != AST_VARIABLE_SLOT || (*block[i]->assignment.variables[j].slotScope)->name.size()) {
					block[i]->assignment.forwardDeclaration = true;
					continue;
				}

				declarations.emplace_back(&block[i]->assignment.variables[j]);
				(*block[i]->assignment.variables[j].slotScope)->name = "var_" + std::to_string(minimizeDiffs ? function.level : function.id) + "_" + std::to_string(variableCounter);
				variableCounter++;
			}

			if (declarations.size()) {
				block.emplace(block.begin() + i, nullptr);
				i++;

				for (uint8_t j = 0; j < declarations.size(); j++) {
					declarationTarget = &block[i - 1];

					for (BlockInfo* currentBlockInfo = &blockInfo; currentBlockInfo->previousBlock;) {
						currentBlockInfo = currentBlockInfo->previousBlock;
						if (currentBlockInfo->index == currentBlockInfo->block.size() - 1) continue;

						switch (currentBlockInfo->block[currentBlockInfo->index]->type) {
						case AST_STATEMENT_IF:
							if (currentBlockInfo->block[currentBlockInfo->index + 1]->type == AST_STATEMENT_ELSE) {
								if ((*declarations[j]->slotScope)->scopeEnd <= currentBlockInfo->block[currentBlockInfo->index]->block.back()->instruction.id) break;
								declarationTarget = &currentBlockInfo->block[currentBlockInfo->index - 1];
								block[i]->assignment.forwardDeclaration = true;
								continue;
							}
						case AST_STATEMENT_ELSE:
							switch (currentBlockInfo->block[currentBlockInfo->index + 1]->type) {
							case AST_STATEMENT_EMPTY:
							case AST_STATEMENT_GOTO:
							case AST_STATEMENT_BREAK:
								if (currentBlockInfo->block[currentBlockInfo->index + 1]->instruction.type == Bytecode::BC_OP_JMP) {
									if ((*declarations[j]->slotScope)->scopeEnd < currentBlockInfo->block[currentBlockInfo->index + 1]->instruction.id) break;
									declarationTarget = &currentBlockInfo->block[currentBlockInfo->index - (currentBlockInfo->block[currentBlockInfo->index]->type == AST_STATEMENT_ELSE ? 2 : 1)];
									block[i]->assignment.forwardDeclaration = true;
									continue;
								}
							default:
								if ((*declarations[j]->slotScope)->scopeEnd < function.labels[currentBlockInfo->block[currentBlockInfo->index + 1]->instruction.label].target) break;
								declarationTarget = &currentBlockInfo->block[currentBlockInfo->index - (currentBlockInfo->block[currentBlockInfo->index]->type == AST_STATEMENT_ELSE ? 2 : 1)];
								block[i]->assignment.forwardDeclaration = true;
								continue;
							}

							break;
						}

						break;
					}

					if (!*declarationTarget) {
						*declarationTarget = new_statement(AST_STATEMENT_DECLARATION);
						(*declarationTarget)->assignment.forwardDeclaration = true;
						(*declarationTarget)->instruction.target = (*declarations[j]->slotScope)->scopeBegin;
					}

					(*declarationTarget)->assignment.variables.emplace_back(*declarations[j]);
				}

				if (!block[i]->assignment.forwardDeclaration) {
					block[i]->type = AST_STATEMENT_DECLARATION;
					block[i]->assignment.forwardDeclaration = true;
					block[i]->instruction.target = block[i - 1]->instruction.target;
					block[i - 1] = nullptr;

					while (block[i]->assignment.expressions.size()
						&& block[i]->assignment.expressions.back()->type == AST_EXPRESSION_CONSTANT
						&& block[i]->assignment.expressions.back()->constant->type == AST_CONSTANT_NIL) {
						block[i]->assignment.expressions.pop_back();
					}
				}

				if (!block[i - 1]) {
					i--;
					block.erase(block.begin() + i);
				}

				declarations.clear();
			}

			if (block[i]->type == AST_STATEMENT_ASSIGNMENT) {
				while (block[i]->assignment.expressions.size() > 1
					&& block[i]->assignment.expressions.back()->type == AST_EXPRESSION_CONSTANT
					&& block[i]->assignment.expressions.back()->constant->type == AST_CONSTANT_NIL) {
					block[i]->assignment.expressions.pop_back();
				}
			}
			
			continue;
		case AST_STATEMENT_IF:
			block.emplace(block.begin() + i, nullptr);
			i++;
			blockInfo.index = i;
			clean_up_block(function, block[i]->block, variableCounter, iteratorCounter, &blockInfo);
			if (!block[i]->block.size()
				&& block[i]->assignment.expressions.back()->type == AST_EXPRESSION_CONSTANT
				&& block[i]->assignment.expressions.back()->constant->type == AST_CONSTANT_FALSE)
				block[i]->type = AST_STATEMENT_EMPTY;
			
			if (i != block.size() - 1 && block[i + 1]->type == AST_STATEMENT_ELSE) {
				i++;

				if (block[i - 1]->type == AST_STATEMENT_EMPTY) {
					block[i]->type = AST_STATEMENT_EMPTY;
					block.reserve(block.size() + block[i]->block.size());
					block.insert(block.begin() + i + 1, block[i]->block.begin(), block[i]->block.begin() + block[i]->block.size());
					block[i]->block.clear();
					block[i]->block.shrink_to_fit();
				} else {
					blockInfo.index++;
					clean_up_block(function, block[i]->block, variableCounter, iteratorCounter, &blockInfo);
					blockInfo.index--;

					if (!block[i]->block.size()) block[i]->type = AST_STATEMENT_EMPTY;
				}
			}

			if (block[blockInfo.index - 1]) {
				block[blockInfo.index - 1]->instruction.target = block[blockInfo.index]->instruction.id;
				continue;
			}

			block.erase(block.begin() + blockInfo.index - 1);
			i--;
			continue;
		}
	}

	for (uint32_t i = 0; i < block.size(); i++) {
		if (function.is_valid_label(block[i]->instruction.label)) {
			block.emplace(block.begin() + i, new_statement(AST_STATEMENT_LABEL));
			block[i]->instruction.label = block[i + 1]->instruction.label;
			i++;
		}

		switch (block[i]->type) {
		case AST_STATEMENT_EMPTY:
			block.erase(block.begin() + i);
			i--;
			continue;
		case AST_STATEMENT_GOTO:
			block[i]->instruction.label = function.get_label_from_id(block[i]->instruction.target);
			continue;
		}
	}

	std::vector<uint32_t> labels;

	for (uint32_t i = block.size(); i--;) {
		if (block[i]->type != AST_STATEMENT_DECLARATION) continue;

		if (block[i]->assignment.forwardDeclaration) {
			for (uint32_t j = i; j < block.size(); j++) {
				if (block[j]->type != AST_STATEMENT_LABEL) continue;

				if (function.labels[block[j]->instruction.label].jumpIds.front() < block[i]->instruction.target) {
					for (uint32_t k = labels.size(); k--;) {
						if (function.labels[block[labels[k]]->instruction.label].jumpIds.back() >= function.labels[block[j]->instruction.label].target) j = labels[k];
					}

					block.emplace(block.begin() + i, new_statement(AST_STATEMENT_DO));
					j++;
					block[i]->block.reserve(j - 1 - i);
					block[i]->block.insert(block[i]->block.begin(), block.begin() + i + 1, block.begin() + j);
					block.erase(block.begin() + i + 1, block.begin() + j);

					if (block[i]->block.size() && block[i]->block.back()->type == AST_STATEMENT_DO) {
						block[i]->block.reserve(block[i]->block.size() + block[i]->block.back()->block.size());
						block[i]->block.insert(block[i]->block.begin() + block[i]->block.size() - 1, block[i]->block.back()->block.begin(), block[i]->block.back()->block.begin() + block[i]->block.back()->block.size());
						block[i]->block.back()->block.clear();
						block[i]->block.back()->block.shrink_to_fit();
						block[i]->block.pop_back();
					}

					break;
				}

				if (function.labels[block[j]->instruction.label].jumpIds.back() > function.labels[block[j]->instruction.label].target) labels.emplace_back(j);
			}

			labels.clear();
			continue;
		}

		if (i == block.size() - 1) {
			block.reserve(block.size() + block[i]->block.size());
			block.insert(block.begin() + block.size(), block[i]->block.begin(), block[i]->block.begin() + block[i]->block.size());
			block[i]->block.clear();
			block[i]->block.shrink_to_fit();
			continue;
		}

		block[i]->block.emplace(block[i]->block.begin(), new_statement(AST_STATEMENT_DECLARATION));
		block[i]->block.front()->assignment.variables = block[i]->assignment.variables;
		block[i]->block.front()->assignment.expressions = block[i]->assignment.expressions;
		block[i]->type = AST_STATEMENT_DO;
	}
}
