/*
 * ast_internal.h — 拆分后的 AST pass 之间共享的内部辅助函数声明。
 * 注意: 本头文件不包含 main.h, 使用前必须先包含 "../main.h"
 * (项目头文件尚无 include guard)。
 */

bool expression_references_scope(const Ast::Expression* expression, const Ast::SlotScope* scope);
bool expression_references_slot(const Ast::Expression* expression, uint8_t slot);
bool expression_has_side_effects(const Ast::Expression* expression);
bool expression_is_copy_safe(const Ast::Expression* expression);
uint32_t count_scope_reads_in_block(const std::vector<Ast::Statement*>& block, const Ast::SlotScope* scope);
void collect_scope_reads(const Ast::Function& function, std::unordered_map<const Ast::SlotScope*, uint32_t>& refCounts);
void collect_written_scopes(const Ast::Function& function, std::unordered_set<const Ast::SlotScope*>& writtenScopes);
std::vector<uint32_t> compute_cfg_idom(const Bytecode::Prototype& prototype);
void collect_captured_scopes(const Ast::Function& function, std::unordered_set<const Ast::SlotScope*>& capturedScopes);
