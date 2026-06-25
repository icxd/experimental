#include <algorithm>
#include <cctype>
#include <set>

#include <lsp/ide.hpp>
#include <lsp/format.hpp>
#include <module.hpp>

namespace {

LspRange make_range(std::string_view source, size_t start, size_t end) {
  auto [sl, sc] = offset_to_line_column(source, start);
  auto [el, ec] = offset_to_line_column(source, end);
  return LspRange{.start_line = sl,
                  .start_character = sc,
                  .end_line = el,
                  .end_character = ec};
}

bool ranges_overlap(const LspRange &a, const LspRange &b) {
  if (a.start_line > b.end_line || b.start_line > a.end_line)
    return false;
  if (a.start_line == a.end_line && b.start_line == b.end_line)
    return a.start_character < b.end_character &&
           b.start_character < a.end_character;
  return true;
}

bool diagnostic_matches_file(const ProjectDiagnostic &diag,
                             std::string_view rel, std::string_view abs_path) {
  return diag.file_path == rel || diag.file_path == abs_path;
}

std::string type_to_string(Type *type) {
  if (type == nullptr)
    return "<unknown>";
  return type->to_string();
}

std::string proc_signature(const CheckedProc &proc) {
  std::string out = std::format("proc {}(", proc.name);
  for (size_t i = 0; i < proc.params.size(); i++) {
    if (i > 0)
      out += ", ";
    out += std::format("{} {}", proc.params[i].name,
                       type_to_string(proc.params[i].type));
  }
  out += ")";
  if (proc.ret_type != nullptr && proc.ret_type->type != TYPE_VOID)
    out += std::format(" {}", type_to_string(proc.ret_type));
  return out;
}

const ParsedModule *module_for_path(const Project &project,
                                    std::string_view abs_path) {
  std::string key = project_abs_path(std::string(abs_path));
  for (const auto &[path, module]: project.modules()) {
    if (path == key)
      return &module;
  }
  return nullptr;
}

Decl *find_top_level_decl(ParsedModule &module, std::string_view name,
                          DeclType kind) {
  for (Decl *decl: module.decls) {
    if (decl->type != kind)
      continue;
    switch (kind) {
    case DECL_PROC: {
      auto *proc = std::get<decl::Proc *>(decl->data);
      if (proc->name.id_value == name)
        return decl;
      break;
    }
    case DECL_STRUCT: {
      auto *strukt = std::get<decl::Struct *>(decl->data);
      if (strukt->name.id_value == name)
        return decl;
      break;
    }
    case DECL_ENUM: {
      auto *enum_ = std::get<decl::Enum *>(decl->data);
      if (enum_->name.id_value == name)
        return decl;
      break;
    }
    case DECL_CONST: {
      auto *konst = std::get<decl::Const *>(decl->data);
      if (konst->name.id_value == name)
        return decl;
      break;
    }
    default:
      break;
    }
  }
  return nullptr;
}

std::optional<std::string_view>
word_at(const ParsedModule &module, size_t line, size_t character) {
  size_t offset = position_to_offset(module.source, line, character);
  auto span = identifier_span_at(module.source, offset);
  if (!span.has_value())
    return std::nullopt;
  return module.source.substr(span->first, span->second - span->first);
}

bool offset_in_token(size_t offset, size_t start, size_t end) {
  return offset >= start && offset < end;
}

Expr *find_expr_containing_offset(Expr *expr, size_t offset) {
  if (expr == nullptr || offset < expr->start || offset >= expr->end)
    return nullptr;

  switch (expr->type) {
  case EXPR_BINARY: {
    auto *bin = std::get<expr::Binary *>(expr->data);
    if (Expr *inner = find_expr_containing_offset(bin->rhs, offset))
      return inner;
    if (Expr *inner = find_expr_containing_offset(bin->lhs, offset))
      return inner;
    return expr;
  }
  case EXPR_GROUP:
    if (Expr *inner =
            find_expr_containing_offset(std::get<expr::Group *>(expr->data)->expr,
                                        offset))
      return inner;
    return expr;
  case EXPR_NOT:
    if (Expr *inner =
            find_expr_containing_offset(std::get<expr::Not *>(expr->data)->expr,
                                        offset))
      return inner;
    return expr;
  case EXPR_REF:
    if (Expr *inner =
            find_expr_containing_offset(std::get<expr::Ref *>(expr->data)->expr,
                                        offset))
      return inner;
    return expr;
  case EXPR_DEREF:
    if (Expr *inner =
            find_expr_containing_offset(std::get<expr::Deref *>(expr->data)->expr,
                                        offset))
      return inner;
    return expr;
  case EXPR_FIELD: {
    auto *field = std::get<expr::Field *>(expr->data);
    if (offset_in_token(offset, field->field.start, field->field.end))
      return expr;
    if (Expr *inner = find_expr_containing_offset(field->base, offset))
      return inner;
    return expr;
  }
  case EXPR_CALL: {
    auto *call = std::get<expr::Call *>(expr->data);
    for (Expr *arg: call->arguments) {
      if (Expr *inner = find_expr_containing_offset(arg, offset))
        return inner;
    }
    return expr;
  }
  case EXPR_STRUCT_LIT: {
    auto *lit = std::get<expr::StructLit *>(expr->data);
    for (const auto &field: lit->fields) {
      if (Expr *inner = find_expr_containing_offset(field.value, offset))
        return inner;
    }
    return expr;
  }
  case EXPR_CAST: {
    auto *cast_ = std::get<expr::Cast *>(expr->data);
    if (Expr *inner = find_expr_containing_offset(cast_->expr, offset))
      return inner;
    return expr;
  }
  case EXPR_INDEX: {
    auto *index = std::get<expr::Index *>(expr->data);
    if (Expr *inner = find_expr_containing_offset(index->index, offset))
      return inner;
    if (Expr *inner = find_expr_containing_offset(index->base, offset))
      return inner;
    return expr;
  }
  case EXPR_TUPLE: {
    auto *tuple = std::get<expr::TupleLit *>(expr->data);
    for (Expr *element: tuple->elements) {
      if (Expr *inner = find_expr_containing_offset(element, offset))
        return inner;
    }
    return expr;
  }
  default:
    return expr;
  }
}

Expr *find_expr_in_stmts(const std::vector<Stmt *> &stmts, size_t offset) {
  for (Stmt *stmt: stmts) {
    if (offset < stmt->start || offset >= stmt->end)
      continue;
    switch (stmt->type) {
    case STMT_EXPR: {
      auto *expr_stmt = std::get<stmt::ExprStmt *>(stmt->data);
      if (Expr *expr = find_expr_containing_offset(expr_stmt->expr, offset))
        return expr;
      break;
    }
    case STMT_VAR: {
      auto *var = std::get<stmt::Var *>(stmt->data);
      if (var->value.has_value()) {
        if (Expr *expr = find_expr_containing_offset(var->value.value(), offset))
          return expr;
      }
      break;
    }
    case STMT_RETURN: {
      auto *ret = std::get<stmt::Return *>(stmt->data);
      if (ret->value.has_value()) {
        if (Expr *expr = find_expr_containing_offset(ret->value.value(), offset))
          return expr;
      }
      break;
    }
    case STMT_ASSIGN: {
      auto *assign = std::get<stmt::Assign *>(stmt->data);
      if (Expr *expr = find_expr_containing_offset(assign->value, offset))
        return expr;
      break;
    }
    case STMT_IF: {
      auto *iff = std::get<stmt::If *>(stmt->data);
      if (Expr *expr = find_expr_containing_offset(iff->cond, offset))
        return expr;
      if (Expr *expr = find_expr_in_stmts(iff->then_block, offset))
        return expr;
      if (Expr *expr = find_expr_in_stmts(iff->else_block, offset))
        return expr;
      break;
    }
    case STMT_WHILE: {
      auto *loop = std::get<stmt::While *>(stmt->data);
      if (Expr *expr = find_expr_containing_offset(loop->cond, offset))
        return expr;
      if (Expr *expr = find_expr_in_stmts(loop->body, offset))
        return expr;
      break;
    }
    case STMT_FOR: {
      auto *loop = std::get<stmt::For *>(stmt->data);
      if (loop->init != nullptr) {
        if (Expr *expr = find_expr_in_stmts({loop->init}, offset))
          return expr;
      }
      if (Expr *expr = find_expr_containing_offset(loop->cond, offset))
        return expr;
      if (loop->step != nullptr) {
        if (Expr *expr = find_expr_in_stmts({loop->step}, offset))
          return expr;
      }
      if (Expr *expr = find_expr_in_stmts(loop->body, offset))
        return expr;
      break;
    }
    case STMT_BLOCK: {
      auto *block = std::get<stmt::Block *>(stmt->data);
      if (Expr *expr = find_expr_in_stmts(block->stmts, offset))
        return expr;
      break;
    }
    default:
      break;
    }
  }
  return nullptr;
}

Expr *find_expr_in_module(const ParsedModule &module, size_t offset) {
  for (Decl *decl: module.decls) {
    if (decl->type != DECL_PROC)
      continue;
    auto *proc = std::get<decl::Proc *>(decl->data);
    if (Expr *expr = find_expr_in_stmts(proc->body, offset))
      return expr;
  }
  return nullptr;
}

Expr *stmt_expr_root_at_offset(Stmt *stmt, size_t offset) {
  if (offset < stmt->start || offset >= stmt->end)
    return nullptr;
  switch (stmt->type) {
  case STMT_EXPR:
    return std::get<stmt::ExprStmt *>(stmt->data)->expr;
  case STMT_VAR: {
    auto *var = std::get<stmt::Var *>(stmt->data);
    return var->value.has_value() ? var->value.value() : nullptr;
  }
  case STMT_RETURN: {
    auto *ret = std::get<stmt::Return *>(stmt->data);
    return ret->value.has_value() ? ret->value.value() : nullptr;
  }
  case STMT_ASSIGN:
    return std::get<stmt::Assign *>(stmt->data)->value;
  case STMT_IF:
    return std::get<stmt::If *>(stmt->data)->cond;
  case STMT_WHILE:
    return std::get<stmt::While *>(stmt->data)->cond;
  case STMT_FOR:
    return std::get<stmt::For *>(stmt->data)->cond;
  default:
    return nullptr;
  }
}

Expr *find_stmt_expr_root_in_stmts(const std::vector<Stmt *> &stmts,
                                   size_t offset) {
  for (Stmt *stmt: stmts) {
    if (offset < stmt->start || offset >= stmt->end) {
      continue;
    }
    if (Expr *root = stmt_expr_root_at_offset(stmt, offset))
      return root;
    switch (stmt->type) {
    case STMT_IF: {
      auto *iff = std::get<stmt::If *>(stmt->data);
      if (Expr *root = find_stmt_expr_root_in_stmts(iff->then_block, offset))
        return root;
      return find_stmt_expr_root_in_stmts(iff->else_block, offset);
    }
    case STMT_WHILE:
      return find_stmt_expr_root_in_stmts(
          std::get<stmt::While *>(stmt->data)->body, offset);
    case STMT_FOR: {
      auto *loop = std::get<stmt::For *>(stmt->data);
      if (loop->init != nullptr) {
        if (Expr *root = find_stmt_expr_root_in_stmts({loop->init}, offset))
          return root;
      }
      if (loop->step != nullptr) {
        if (Expr *root = find_stmt_expr_root_in_stmts({loop->step}, offset))
          return root;
      }
      return find_stmt_expr_root_in_stmts(loop->body, offset);
    }
    case STMT_BLOCK:
      return find_stmt_expr_root_in_stmts(
          std::get<stmt::Block *>(stmt->data)->stmts, offset);
    default:
      break;
    }
  }
  return nullptr;
}

Expr *find_stmt_expr_root_in_module(const ParsedModule &module, size_t offset) {
  for (Decl *decl: module.decls) {
    if (decl->type != DECL_PROC)
      continue;
    auto *proc = std::get<decl::Proc *>(decl->data);
    if (Expr *root = find_stmt_expr_root_in_stmts(proc->body, offset))
      return root;
  }
  return nullptr;
}

std::optional<CheckedEnum>
find_registry_enum(const ModuleRegistry &registry, std::string_view module,
                   std::string_view name) {
  if (auto enum_ = registry.find_enum(module, name); enum_.has_value())
    return enum_;
  for (const auto &entry: registry.modules()) {
    if (auto enum_ = registry.find_enum(entry.first, name); enum_.has_value())
      return enum_;
  }
  return std::nullopt;
}

Type *enum_type_for_enum_case_in_tree(const ModuleRegistry &registry,
                                      std::string_view module_name, Expr *root,
                                      const Expr *case_expr) {
  if (root == nullptr)
    return nullptr;

  if (root->type == EXPR_BINARY) {
    auto *bin = std::get<expr::Binary *>(root->data);
    if (bin->rhs == case_expr && bin->lhs->expr_type != nullptr &&
        bin->lhs->expr_type->type == TYPE_ENUM)
      return bin->lhs->expr_type;
    if (bin->lhs == case_expr && bin->rhs->expr_type != nullptr &&
        bin->rhs->expr_type->type == TYPE_ENUM)
      return bin->rhs->expr_type;
    Type *lhs_type =
        enum_type_for_enum_case_in_tree(registry, module_name, bin->lhs,
                                        case_expr);
    if (lhs_type != nullptr)
      return lhs_type;
    return enum_type_for_enum_case_in_tree(registry, module_name, bin->rhs,
                                           case_expr);
  }

  if (root->type == EXPR_STRUCT_LIT) {
    auto *lit = std::get<expr::StructLit *>(root->data);
    for (const auto &field: lit->fields) {
      if (field.value != case_expr)
        continue;
      std::optional<CheckedStruct> strukt =
          registry.find_struct(module_name, lit->type_name.id_value);
      if (!strukt.has_value()) {
        for (const auto &entry: registry.modules()) {
          strukt = registry.find_struct(entry.first, lit->type_name.id_value);
          if (strukt.has_value())
            break;
        }
      }
      if (strukt.has_value()) {
        for (const auto &struct_field: strukt->fields) {
          if (struct_field.name == field.name.id_value &&
              struct_field.type->type == TYPE_ENUM)
            return struct_field.type;
        }
      }
    }
    for (const auto &field: lit->fields) {
      Type *type =
          enum_type_for_enum_case_in_tree(registry, module_name, field.value,
                                          case_expr);
      if (type != nullptr)
        return type;
    }
  }

  if (root->type == EXPR_GROUP)
    return enum_type_for_enum_case_in_tree(
        registry, module_name,
        std::get<expr::Group *>(root->data)->expr, case_expr);
  if (root->type == EXPR_NOT)
    return enum_type_for_enum_case_in_tree(
        registry, module_name, std::get<expr::Not *>(root->data)->expr,
        case_expr);
  if (root->type == EXPR_CAST)
    return enum_type_for_enum_case_in_tree(
        registry, module_name, std::get<expr::Cast *>(root->data)->expr,
        case_expr);

  return nullptr;
}

void add_enum_member_completions(const ModuleRegistry &registry,
                                 std::string_view module_name,
                                 std::string_view enum_name,
                                 std::string_view prefix,
                                 std::vector<CompletionItem> &items,
                                 const auto &add_unique) {
  if (auto enum_ = find_registry_enum(registry, module_name, enum_name);
      enum_.has_value()) {
    for (const auto &member: enum_->members) {
      if (member.name == "count")
        continue;
      if (!prefix.empty() && !member.name.starts_with(prefix))
        continue;
      add_unique(CompletionItem{
          .label = std::string(member.name),
          .detail = std::format("enum {}", enum_name),
          .insert_text = std::string(member.name),
          .kind = 21,
      });
    }
  }
}

Expr *find_call_at_offset(Expr *expr, size_t offset) {
  if (expr == nullptr)
    return nullptr;

  if (offset < expr->start || offset >= expr->end)
    return nullptr;

  if (expr->type == EXPR_CALL)
    return expr;

  switch (expr->type) {
  case EXPR_BINARY: {
    auto *bin = std::get<expr::Binary *>(expr->data);
    if (Expr *found = find_call_at_offset(bin->lhs, offset))
      return found;
    return find_call_at_offset(bin->rhs, offset);
  }
  case EXPR_GROUP:
    return find_call_at_offset(std::get<expr::Group *>(expr->data)->expr,
                               offset);
  case EXPR_NOT:
    return find_call_at_offset(std::get<expr::Not *>(expr->data)->expr, offset);
  case EXPR_REF:
    return find_call_at_offset(std::get<expr::Ref *>(expr->data)->expr, offset);
  case EXPR_DEREF:
    return find_call_at_offset(std::get<expr::Deref *>(expr->data)->expr,
                               offset);
  case EXPR_FIELD: {
    auto *field = std::get<expr::Field *>(expr->data);
    if (offset_in_token(offset, field->field.start, field->field.end))
      return nullptr;
    return find_call_at_offset(field->base, offset);
  }
  case EXPR_INDEX: {
    auto *index = std::get<expr::Index *>(expr->data);
    return find_call_at_offset(index->base, offset);
  }
  case EXPR_CAST: {
    auto *cast_ = std::get<expr::Cast *>(expr->data);
    return find_call_at_offset(cast_->expr, offset);
  }
  default:
    return nullptr;
  }
}

Expr *find_call_in_stmts(const std::vector<Stmt *> &stmts, size_t offset) {
  for (Stmt *stmt: stmts) {
    if (offset < stmt->start || offset >= stmt->end)
      continue;
    switch (stmt->type) {
    case STMT_EXPR: {
      auto *expr_stmt = std::get<stmt::ExprStmt *>(stmt->data);
      if (Expr *call = find_call_at_offset(expr_stmt->expr, offset))
        return call;
      break;
    }
    case STMT_VAR: {
      auto *var = std::get<stmt::Var *>(stmt->data);
      if (var->value.has_value()) {
        if (Expr *call = find_call_at_offset(var->value.value(), offset))
          return call;
      }
      break;
    }
    case STMT_RETURN: {
      auto *ret = std::get<stmt::Return *>(stmt->data);
      if (ret->value.has_value()) {
        if (Expr *call = find_call_at_offset(ret->value.value(), offset))
          return call;
      }
      break;
    }
    case STMT_ASSIGN: {
      auto *assign = std::get<stmt::Assign *>(stmt->data);
      if (Expr *call = find_call_at_offset(assign->value, offset))
        return call;
      break;
    }
    case STMT_IF: {
      auto *iff = std::get<stmt::If *>(stmt->data);
      if (Expr *call = find_call_at_offset(iff->cond, offset))
        return call;
      if (Expr *call = find_call_in_stmts(iff->then_block, offset))
        return call;
      if (Expr *call = find_call_in_stmts(iff->else_block, offset))
        return call;
      break;
    }
    case STMT_WHILE: {
      auto *loop = std::get<stmt::While *>(stmt->data);
      if (Expr *call = find_call_at_offset(loop->cond, offset))
        return call;
      if (Expr *call = find_call_in_stmts(loop->body, offset))
        return call;
      break;
    }
    case STMT_FOR: {
      auto *loop = std::get<stmt::For *>(stmt->data);
      if (Expr *call = find_call_at_offset(loop->cond, offset))
        return call;
      if (Expr *call = find_call_in_stmts(loop->body, offset))
        return call;
      break;
    }
    case STMT_BLOCK: {
      auto *block = std::get<stmt::Block *>(stmt->data);
      if (Expr *call = find_call_in_stmts(block->stmts, offset))
        return call;
      break;
    }
    default:
      break;
    }
  }
  return nullptr;
}

Expr *find_call_in_module(const ParsedModule &module, size_t offset) {
  for (Decl *decl: module.decls) {
    if (decl->type != DECL_PROC)
      continue;
    auto *proc = std::get<decl::Proc *>(decl->data);
    if (Expr *call = find_call_in_stmts(proc->body, offset))
      return call;
  }
  return nullptr;
}

Expr *find_expr_at_offset(Expr *expr, size_t offset) {
  if (expr == nullptr || offset < expr->start || offset >= expr->end)
    return nullptr;

  switch (expr->type) {
  case EXPR_BINARY: {
    auto *bin = std::get<expr::Binary *>(expr->data);
    if (Expr *inner = find_expr_at_offset(bin->rhs, offset))
      return inner;
    return find_expr_at_offset(bin->lhs, offset);
  }
  case EXPR_GROUP:
    return find_expr_at_offset(std::get<expr::Group *>(expr->data)->expr,
                               offset);
  case EXPR_NOT:
    return find_expr_at_offset(std::get<expr::Not *>(expr->data)->expr,
                               offset);
  case EXPR_REF:
    return find_expr_at_offset(std::get<expr::Ref *>(expr->data)->expr,
                               offset);
  case EXPR_DEREF:
    return find_expr_at_offset(std::get<expr::Deref *>(expr->data)->expr,
                               offset);
  case EXPR_FIELD: {
    auto *field = std::get<expr::Field *>(expr->data);
    if (offset_in_token(offset, field->field.start, field->field.end))
      return expr;
    return find_expr_at_offset(field->base, offset);
  }
  case EXPR_VAR:
  case EXPR_CALL:
  case EXPR_INDEX:
  case EXPR_CAST:
  case EXPR_ENUM_CASE:
    return expr;
  default:
    return expr;
  }
}

bool is_after_dot(std::string_view source, size_t offset) {
  size_t i = offset;
  while (i > 0 && std::isspace(static_cast<unsigned char>(source[i - 1])))
    i--;
  return i > 0 && source[i - 1] == '.';
}

std::optional<std::string_view>
proc_containing_offset(const ParsedModule &module, size_t offset) {
  for (Decl *decl: module.decls) {
    if (decl->type != DECL_PROC)
      continue;
    if (offset >= decl->start && offset < decl->end) {
      auto *proc = std::get<decl::Proc *>(decl->data);
      return proc->name.id_value;
    }
  }
  return std::nullopt;
}

enum class SymKind { Proc, Struct, Enum, Const, LocalVar, Param, StructField };

struct Sym {
  SymKind kind;
  std::string module_name;
  std::string name;
  std::string struct_name;
  std::string proc_name;
  std::string file_path;
  size_t def_start = 0;
  size_t def_end = 0;
  bool renamable = true;
};

bool var_has_inferred_type(std::string_view source, const stmt::Var *var) {
  if (!var->value.has_value())
    return false;
  size_t pos = var->name.end;
  while (pos < var->value.value()->start &&
         std::isspace(static_cast<unsigned char>(source[pos])))
    pos++;
  return pos < var->value.value()->start && source[pos] == '=';
}

bool inlay_hint_in_range(const InlayHint &hint, const LspRange &range) {
  if (hint.line < range.start_line || hint.line > range.end_line)
    return false;
  if (hint.line == range.start_line && hint.character < range.start_character)
    return false;
  if (hint.line == range.end_line && hint.character >= range.end_character)
    return false;
  return true;
}

std::optional<Sym> find_sym_in_stmts(const ParsedModule &module,
                                     std::string_view proc_name,
                                     const std::vector<Stmt *> &stmts,
                                     size_t offset) {
  for (Stmt *stmt: stmts) {
    switch (stmt->type) {
    case STMT_VAR: {
      auto *var = std::get<stmt::Var *>(stmt->data);
      if (offset_in_token(offset, var->name.start, var->name.end)) {
        return Sym{.kind = SymKind::LocalVar,
                   .module_name = std::string(module.module_name),
                   .name = std::string(var->name.id_value),
                   .proc_name = std::string(proc_name),
                   .file_path = module.abs_path,
                   .def_start = var->name.start,
                   .def_end = var->name.end};
      }
      break;
    }
    case STMT_IF: {
      auto *iff = std::get<stmt::If *>(stmt->data);
      if (auto sym = find_sym_in_stmts(module, proc_name, iff->then_block, offset))
        return sym;
      if (auto sym = find_sym_in_stmts(module, proc_name, iff->else_block, offset))
        return sym;
      break;
    }
    case STMT_WHILE: {
      auto *loop = std::get<stmt::While *>(stmt->data);
      if (auto sym = find_sym_in_stmts(module, proc_name, loop->body, offset))
        return sym;
      break;
    }
    case STMT_FOR: {
      auto *loop = std::get<stmt::For *>(stmt->data);
      if (loop->init != nullptr) {
        if (auto sym = find_sym_in_stmts(module, proc_name, {loop->init}, offset))
          return sym;
      }
      if (auto sym = find_sym_in_stmts(module, proc_name, loop->body, offset))
        return sym;
      break;
    }
    case STMT_BLOCK: {
      auto *block = std::get<stmt::Block *>(stmt->data);
      if (auto sym = find_sym_in_stmts(module, proc_name, block->stmts, offset))
        return sym;
      break;
    }
    case STMT_WHEN: {
      auto *when = std::get<stmt::When *>(stmt->data);
      if (auto sym = find_sym_in_stmts(module, proc_name, when->true_block, offset))
        return sym;
      if (auto sym = find_sym_in_stmts(module, proc_name, when->false_block, offset))
        return sym;
      break;
    }
    default:
      break;
    }
  }
  return std::nullopt;
}

std::optional<Sym> find_local_var_sym_in_stmts(const ParsedModule &module,
                                               std::string_view proc_name,
                                               const std::vector<Stmt *> &stmts,
                                               std::string_view name) {
  for (Stmt *stmt: stmts) {
    switch (stmt->type) {
    case STMT_VAR: {
      auto *var = std::get<stmt::Var *>(stmt->data);
      if (var->name.id_value == name) {
        return Sym{.kind = SymKind::LocalVar,
                   .module_name = std::string(module.module_name),
                   .name = std::string(var->name.id_value),
                   .proc_name = std::string(proc_name),
                   .file_path = module.abs_path,
                   .def_start = var->name.start,
                   .def_end = var->name.end};
      }
      break;
    }
    case STMT_IF: {
      auto *iff = std::get<stmt::If *>(stmt->data);
      if (auto sym = find_local_var_sym_in_stmts(module, proc_name, iff->then_block, name))
        return sym;
      if (auto sym = find_local_var_sym_in_stmts(module, proc_name, iff->else_block, name))
        return sym;
      break;
    }
    case STMT_WHILE: {
      auto *loop = std::get<stmt::While *>(stmt->data);
      if (auto sym = find_local_var_sym_in_stmts(module, proc_name, loop->body, name))
        return sym;
      break;
    }
    case STMT_FOR: {
      auto *loop = std::get<stmt::For *>(stmt->data);
      if (loop->init != nullptr) {
        if (auto sym = find_local_var_sym_in_stmts(module, proc_name, {loop->init}, name))
          return sym;
      }
      if (auto sym = find_local_var_sym_in_stmts(module, proc_name, loop->body, name))
        return sym;
      break;
    }
    case STMT_BLOCK: {
      auto *block = std::get<stmt::Block *>(stmt->data);
      if (auto sym = find_local_var_sym_in_stmts(module, proc_name, block->stmts, name))
        return sym;
      break;
    }
    case STMT_WHEN: {
      auto *when = std::get<stmt::When *>(stmt->data);
      if (auto sym = find_local_var_sym_in_stmts(module, proc_name, when->true_block, name))
        return sym;
      if (auto sym = find_local_var_sym_in_stmts(module, proc_name, when->false_block, name))
        return sym;
      break;
    }
    default:
      break;
    }
  }
  return std::nullopt;
}

void collect_inlay_hints_in_expr(const Project &project, const ParsedModule &mod,
                                 Expr *expr, const IdeInlayHintOptions &options,
                                 std::vector<InlayHint> &out) {
  if (expr == nullptr)
    return;

  if (expr->type == EXPR_CALL) {
    auto *call = std::get<expr::Call *>(expr->data);
    if (!call->name.has_value())
      return;

    std::string_view module_name = mod.module_name;
    if (call->module.has_value())
      module_name = call->module->id_value;
    else if (call->resolved_module.has_value())
      module_name = *call->resolved_module;

    auto proc = project.registry().find_proc(module_name, call->name->id_value);
    if (options.parameter_names && proc.has_value()) {
      size_t paren = call->name->end;
      while (paren < mod.source.size() && mod.source[paren] != '(')
        paren++;
      if (paren < mod.source.size()) {
        size_t param_index = 0;
        if (call->receiver != nullptr)
          param_index++;

        std::vector<size_t> arg_positions{};
        for (size_t scan = paren; scan < expr->end; scan++) {
          char ch = mod.source[scan];
          if (ch == '(' || ch == ',') {
            size_t pos = scan + 1;
            while (pos < expr->end &&
                   std::isspace(static_cast<unsigned char>(mod.source[pos])))
              pos++;
            arg_positions.push_back(pos);
          } else if (ch == ')') {
            break;
          }
        }

        for (size_t i = 0; i < arg_positions.size(); i++) {
          size_t pidx = param_index + i;
          if (pidx >= proc->params.size())
            break;
          auto [line, character] =
              offset_to_line_column(mod.source, arg_positions[i]);
          out.push_back(InlayHint{
              .line = line,
              .character = character,
              .label = std::format("{}: ", proc->params[pidx].name),
              .kind = 2,
          });
        }
      }
    }

    if (call->receiver != nullptr)
      collect_inlay_hints_in_expr(project, mod, call->receiver, options, out);
    for (Expr *arg: call->arguments)
      collect_inlay_hints_in_expr(project, mod, arg, options, out);
    return;
  }

  switch (expr->type) {
  case EXPR_BINARY: {
    auto *bin = std::get<expr::Binary *>(expr->data);
    collect_inlay_hints_in_expr(project, mod, bin->lhs, options, out);
    collect_inlay_hints_in_expr(project, mod, bin->rhs, options, out);
    return;
  }
  case EXPR_GROUP:
    collect_inlay_hints_in_expr(project, mod,
                                std::get<expr::Group *>(expr->data)->expr,
                                options, out);
    return;
  case EXPR_NOT:
    collect_inlay_hints_in_expr(project, mod,
                                std::get<expr::Not *>(expr->data)->expr,
                                options, out);
    return;
  case EXPR_REF:
    collect_inlay_hints_in_expr(project, mod,
                                std::get<expr::Ref *>(expr->data)->expr,
                                options, out);
    return;
  case EXPR_DEREF:
    collect_inlay_hints_in_expr(project, mod,
                                std::get<expr::Deref *>(expr->data)->expr,
                                options, out);
    return;
  case EXPR_FIELD: {
    auto *field = std::get<expr::Field *>(expr->data);
    collect_inlay_hints_in_expr(project, mod, field->base, options, out);
    return;
  }
  case EXPR_INDEX: {
    auto *index = std::get<expr::Index *>(expr->data);
    collect_inlay_hints_in_expr(project, mod, index->base, options, out);
    collect_inlay_hints_in_expr(project, mod, index->index, options, out);
    return;
  }
  case EXPR_CAST: {
    auto *cast_ = std::get<expr::Cast *>(expr->data);
    collect_inlay_hints_in_expr(project, mod, cast_->expr, options, out);
    return;
  }
  case EXPR_STRUCT_LIT: {
    auto *lit = std::get<expr::StructLit *>(expr->data);
    for (const auto &f: lit->fields)
      collect_inlay_hints_in_expr(project, mod, f.value, options, out);
    return;
  }
  default:
    break;
  }
}

void collect_inlay_hints_in_stmts(const Project &project, const ParsedModule &mod,
                                  const std::vector<Stmt *> &stmts,
                                  const IdeInlayHintOptions &options,
                                  std::vector<InlayHint> &out) {
  for (Stmt *stmt: stmts) {
    switch (stmt->type) {
    case STMT_EXPR:
      collect_inlay_hints_in_expr(
          project, mod, std::get<stmt::ExprStmt *>(stmt->data)->expr, options,
          out);
      break;
    case STMT_VAR: {
      auto *var = std::get<stmt::Var *>(stmt->data);
      if (options.variable_types && var->value.has_value() &&
          var_has_inferred_type(mod.source, var)) {
        Type *type = var->type;
        if (type == nullptr && var->value.value()->expr_type != nullptr)
          type = var->value.value()->expr_type;
        if (type != nullptr) {
          auto [line, character] =
              offset_to_line_column(mod.source, var->name.end);
          out.push_back(InlayHint{
              .line = line,
              .character = character,
              .label = std::format(": {}", type_to_string(type)),
              .kind = 1,
              .padding_left = false,
              .padding_right = false,
          });
        }
      }
      if (var->value.has_value())
        collect_inlay_hints_in_expr(project, mod, var->value.value(), options,
                                    out);
      break;
    }
    case STMT_RETURN: {
      auto *ret = std::get<stmt::Return *>(stmt->data);
      if (ret->value.has_value())
        collect_inlay_hints_in_expr(project, mod, ret->value.value(), options,
                                    out);
      break;
    }
    case STMT_ASSIGN: {
      auto *assign = std::get<stmt::Assign *>(stmt->data);
      collect_inlay_hints_in_expr(project, mod, assign->value, options, out);
      break;
    }
    case STMT_IF: {
      auto *iff = std::get<stmt::If *>(stmt->data);
      collect_inlay_hints_in_expr(project, mod, iff->cond, options, out);
      collect_inlay_hints_in_stmts(project, mod, iff->then_block, options, out);
      collect_inlay_hints_in_stmts(project, mod, iff->else_block, options, out);
      break;
    }
    case STMT_WHILE: {
      auto *loop = std::get<stmt::While *>(stmt->data);
      collect_inlay_hints_in_expr(project, mod, loop->cond, options, out);
      collect_inlay_hints_in_stmts(project, mod, loop->body, options, out);
      break;
    }
    case STMT_FOR: {
      auto *loop = std::get<stmt::For *>(stmt->data);
      if (loop->init != nullptr)
        collect_inlay_hints_in_stmts(project, mod, {loop->init}, options, out);
      collect_inlay_hints_in_expr(project, mod, loop->cond, options, out);
      if (loop->step != nullptr)
        collect_inlay_hints_in_stmts(project, mod, {loop->step}, options, out);
      collect_inlay_hints_in_stmts(project, mod, loop->body, options, out);
      break;
    }
    case STMT_BLOCK:
      collect_inlay_hints_in_stmts(
          project, mod, std::get<stmt::Block *>(stmt->data)->stmts, options,
          out);
      break;
    case STMT_WHEN: {
      auto *when = std::get<stmt::When *>(stmt->data);
      collect_inlay_hints_in_expr(project, mod, when->cond, options, out);
      collect_inlay_hints_in_stmts(project, mod, when->true_block, options, out);
      collect_inlay_hints_in_stmts(project, mod, when->false_block, options, out);
      break;
    }
    default:
      break;
    }
  }
}

std::optional<Sym> resolve_symbol(const Project &project,
                                  std::string_view abs_path, size_t line,
                                  size_t character) {
  const ParsedModule *module = module_for_path(project, abs_path);
  if (module == nullptr)
    return std::nullopt;

  auto word = word_at(*module, line, character);
  if (!word.has_value())
    return std::nullopt;

  size_t offset = position_to_offset(module->source, line, character);

  for (Decl *decl: module->decls) {
    if (decl->type == DECL_PROC) {
      auto *proc = std::get<decl::Proc *>(decl->data);
      if (offset_in_token(offset, proc->name.start, proc->name.end)) {
        return Sym{.kind = SymKind::Proc,
                   .module_name = std::string(module->module_name),
                   .name = std::string(proc->name.id_value),
                   .file_path = module->abs_path,
                   .def_start = proc->name.start,
                   .def_end = proc->name.end,
                   .renamable = !module->is_runtime};
      }
      for (const Param &param: proc->params) {
        if (offset_in_token(offset, param.name.start, param.name.end)) {
          return Sym{.kind = SymKind::Param,
                     .module_name = std::string(module->module_name),
                     .name = std::string(param.name.id_value),
                     .proc_name = std::string(proc->name.id_value),
                     .file_path = module->abs_path,
                     .def_start = param.name.start,
                     .def_end = param.name.end,
                     .renamable = !module->is_runtime};
        }
      }
    }
    if (decl->type == DECL_STRUCT) {
      auto *strukt = std::get<decl::Struct *>(decl->data);
      if (offset_in_token(offset, strukt->name.start, strukt->name.end)) {
        return Sym{.kind = SymKind::Struct,
                   .module_name = std::string(module->module_name),
                   .name = std::string(strukt->name.id_value),
                   .file_path = module->abs_path,
                   .def_start = strukt->name.start,
                   .def_end = strukt->name.end,
                   .renamable = !module->is_runtime};
      }
      for (const auto &field: strukt->fields) {
        if (offset_in_token(offset, field.name.start, field.name.end)) {
          return Sym{.kind = SymKind::StructField,
                     .module_name = std::string(module->module_name),
                     .name = std::string(field.name.id_value),
                     .struct_name = std::string(strukt->name.id_value),
                     .file_path = module->abs_path,
                     .def_start = field.name.start,
                     .def_end = field.name.end,
                     .renamable = !module->is_runtime};
        }
      }
    }
    if (decl->type == DECL_ENUM) {
      auto *enum_ = std::get<decl::Enum *>(decl->data);
      if (offset_in_token(offset, enum_->name.start, enum_->name.end)) {
        return Sym{.kind = SymKind::Enum,
                   .module_name = std::string(module->module_name),
                   .name = std::string(enum_->name.id_value),
                   .file_path = module->abs_path,
                   .def_start = enum_->name.start,
                   .def_end = enum_->name.end,
                   .renamable = !module->is_runtime};
      }
      for (const auto &member: enum_->members) {
        if (offset_in_token(offset, member.name.start, member.name.end)) {
          return Sym{.kind = SymKind::Enum,
                     .module_name = std::string(module->module_name),
                     .name = std::string(member.name.id_value),
                     .file_path = module->abs_path,
                     .def_start = member.name.start,
                     .def_end = member.name.end,
                     .renamable = !module->is_runtime};
        }
      }
    }
    if (decl->type == DECL_CONST) {
      auto *konst = std::get<decl::Const *>(decl->data);
      if (offset_in_token(offset, konst->name.start, konst->name.end)) {
        return Sym{.kind = SymKind::Const,
                   .module_name = std::string(module->module_name),
                   .name = std::string(konst->name.id_value),
                   .file_path = module->abs_path,
                   .def_start = konst->name.start,
                   .def_end = konst->name.end,
                   .renamable = !module->is_runtime};
      }
    }
  }

  auto enclosing_proc = proc_containing_offset(*module, offset);
  if (enclosing_proc.has_value()) {
    for (Decl *decl: module->decls) {
      if (decl->type != DECL_PROC)
        continue;
      auto *proc = std::get<decl::Proc *>(decl->data);
      if (proc->name.id_value != enclosing_proc.value())
        continue;
      for (const Param &param: proc->params) {
        if (offset_in_token(offset, param.name.start, param.name.end)) {
          return Sym{.kind = SymKind::Param,
                     .module_name = std::string(module->module_name),
                     .name = std::string(param.name.id_value),
                     .proc_name = std::string(proc->name.id_value),
                     .file_path = module->abs_path,
                     .def_start = param.name.start,
                     .def_end = param.name.end,
                     .renamable = !module->is_runtime};
        }
      }
      if (auto sym = find_sym_in_stmts(*module, enclosing_proc.value(), proc->body,
                                        offset))
        return sym;
    }
  }

  Expr *expr = find_expr_in_module(*module, offset);

  if (expr != nullptr && expr->type == EXPR_VAR) {
    auto *var = std::get<expr::Var *>(expr->data);
    if (var->module.has_value()) {
      for (const auto &[path, mod]: project.modules()) {
        if (mod.module_name != var->module->id_value)
          continue;
        if (Decl *found = find_top_level_decl(const_cast<ParsedModule &>(mod),
                                              var->var.id_value, DECL_CONST)) {
          auto *k = std::get<decl::Const *>(found->data);
          return Sym{.kind = SymKind::Const,
                     .module_name = std::string(mod.module_name),
                     .name = std::string(k->name.id_value),
                     .file_path = mod.abs_path,
                     .def_start = k->name.start,
                     .def_end = k->name.end,
                     .renamable = !mod.is_runtime};
        }
      }
    }

    if (!var->module.has_value() && enclosing_proc.has_value()) {
      for (Decl *decl: module->decls) {
        if (decl->type != DECL_PROC)
          continue;
        auto *proc = std::get<decl::Proc *>(decl->data);
        if (proc->name.id_value != enclosing_proc.value())
          continue;
        for (const Param &param: proc->params) {
          if (param.name.id_value == var->var.id_value) {
            return Sym{.kind = SymKind::Param,
                       .module_name = std::string(module->module_name),
                       .name = std::string(param.name.id_value),
                       .proc_name = std::string(proc->name.id_value),
                       .file_path = module->abs_path,
                       .def_start = param.name.start,
                       .def_end = param.name.end,
                       .renamable = !module->is_runtime};
          }
        }
        if (auto local = find_local_var_sym_in_stmts(
                *module, enclosing_proc.value(), proc->body, var->var.id_value))
          return local;
      }
    }
  }

  if (expr != nullptr && expr->type == EXPR_CALL) {
    auto *call = std::get<expr::Call *>(expr->data);
    if (!call->name.has_value())
      return std::nullopt;

    std::string_view module_name = module->module_name;
    if (call->module.has_value())
      module_name = call->module->id_value;
    else if (call->resolved_module.has_value())
      module_name = *call->resolved_module;

    for (const auto &[path, mod]: project.modules()) {
      if (mod.module_name != module_name)
        continue;
      if (Decl *decl = find_top_level_decl(const_cast<ParsedModule &>(mod),
                                           call->name->id_value, DECL_PROC)) {
        auto *proc = std::get<decl::Proc *>(decl->data);
        return Sym{.kind = SymKind::Proc,
                   .module_name = std::string(mod.module_name),
                   .name = std::string(proc->name.id_value),
                   .file_path = mod.abs_path,
                   .def_start = proc->name.start,
                   .def_end = proc->name.end,
                   .renamable = !mod.is_runtime};
      }
    }
  }

  if (expr != nullptr && expr->type == EXPR_FIELD) {
    auto *field = std::get<expr::Field *>(expr->data);
    if (field->base->type == EXPR_VAR) {
      auto *var = std::get<expr::Var *>(field->base->data);
      if (!var->module.has_value()) {
        for (const auto &[path, mod]: project.modules()) {
          if (Decl *decl = find_top_level_decl(const_cast<ParsedModule &>(mod),
                                               var->var.id_value, DECL_ENUM)) {
            auto *enum_ = std::get<decl::Enum *>(decl->data);
            for (const auto &member: enum_->members) {
              if (member.name.id_value == field->field.id_value) {
                return Sym{.kind = SymKind::Enum,
                           .module_name = std::string(mod.module_name),
                           .name = std::string(member.name.id_value),
                           .file_path = mod.abs_path,
                           .def_start = member.name.start,
                           .def_end = member.name.end,
                           .renamable = !mod.is_runtime};
              }
            }
          }
        }
      }
    }
    Type *struct_type = field->base->expr_type != nullptr
                            ? field->base->expr_type->as_struct_for_field_access()
                            : nullptr;
    if (struct_type != nullptr) {
      std::string_view struct_name =
          std::get<type::Struct *>(struct_type->data)->name;
      for (const auto &[path, mod]: project.modules()) {
        if (Decl *decl = find_top_level_decl(const_cast<ParsedModule &>(mod),
                                             struct_name, DECL_STRUCT)) {
          auto *strukt = std::get<decl::Struct *>(decl->data);
          for (const auto &f: strukt->fields) {
            if (f.name.id_value == field->field.id_value) {
              return Sym{.kind = SymKind::StructField,
                         .module_name = std::string(mod.module_name),
                         .name = std::string(f.name.id_value),
                         .struct_name = std::string(struct_name),
                         .file_path = mod.abs_path,
                         .def_start = f.name.start,
                         .def_end = f.name.end,
                         .renamable = !mod.is_runtime};
            }
          }
        }
      }
    }
  }

  if (expr != nullptr && expr->type == EXPR_ENUM_CASE) {
    auto *case_ = std::get<expr::EnumCase *>(expr->data);
    std::string_view member_name = case_->member.id_value;
    std::optional<std::string_view> enum_name;

    if (Expr *root = find_stmt_expr_root_in_module(*module, offset);
        root != nullptr) {
      if (Type *enum_type =
              enum_type_for_enum_case_in_tree(project.registry(),
                                              module->module_name, root, expr);
          enum_type != nullptr && enum_type->type == TYPE_ENUM)
        enum_name = std::get<type::Enum *>(enum_type->data)->name;
    }

    for (Decl *decl: module->decls) {
      if (decl->type != DECL_ENUM)
        continue;
      auto *enum_ = std::get<decl::Enum *>(decl->data);
      if (enum_name.has_value() && enum_->name.id_value != enum_name.value())
        continue;
      for (const auto &member: enum_->members) {
        if (member.name.id_value == member_name) {
          return Sym{.kind = SymKind::Enum,
                     .module_name = std::string(module->module_name),
                     .name = std::string(member.name.id_value),
                     .file_path = module->abs_path,
                     .def_start = member.name.start,
                     .def_end = member.name.end,
                     .renamable = !module->is_runtime};
        }
      }
    }
  }

  for (const auto &[path, mod]: project.modules()) {
    if (Decl *decl = find_top_level_decl(const_cast<ParsedModule &>(mod),
                                         word.value(), DECL_PROC)) {
      auto *proc = std::get<decl::Proc *>(decl->data);
      return Sym{.kind = SymKind::Proc,
                 .module_name = std::string(mod.module_name),
                 .name = std::string(proc->name.id_value),
                 .file_path = mod.abs_path,
                 .def_start = proc->name.start,
                 .def_end = proc->name.end,
                 .renamable = !mod.is_runtime};
    }
  }

  return std::nullopt;
}

bool call_matches_proc(const expr::Call *call, const ParsedModule &mod,
                       const Sym &sym) {
  if (sym.kind != SymKind::Proc || !call->name.has_value())
    return false;
  std::string_view use_module = mod.module_name;
  if (call->module.has_value())
    use_module = call->module->id_value;
  else if (call->resolved_module.has_value())
    use_module = *call->resolved_module;
  return call->name->id_value == sym.name && use_module == sym.module_name;
}

bool var_matches_const(const expr::Var *var, const ParsedModule &mod,
                       const Sym &sym) {
  if (sym.kind != SymKind::Const)
    return false;
  if (var->module.has_value())
    return var->module->id_value == sym.module_name &&
           var->var.id_value == sym.name;
  return mod.module_name == sym.module_name && var->var.id_value == sym.name;
}

bool type_matches_struct(Type *type, const Sym &sym) {
  return type != nullptr && type->type == TYPE_STRUCT &&
         std::get<type::Struct *>(type->data)->name == sym.name &&
         sym.kind == SymKind::Struct;
}

bool type_matches_enum(Type *type, const Sym &sym) {
  return type != nullptr && type->type == TYPE_ENUM &&
         std::get<type::Enum *>(type->data)->name == sym.name &&
         sym.kind == SymKind::Enum;
}

void push_location(std::vector<LspLocation> &out, const ParsedModule &mod,
                   size_t start, size_t end) {
  LspLocation loc{
      .uri = lsp_path_to_uri(mod.abs_path),
      .range = make_range(mod.source, start, end),
  };
  if (std::any_of(out.begin(), out.end(), [&](const LspLocation &existing) {
        return existing.uri == loc.uri &&
               existing.range.start_line == loc.range.start_line &&
               existing.range.start_character == loc.range.start_character &&
               existing.range.end_line == loc.range.end_line &&
               existing.range.end_character == loc.range.end_character;
      }))
    return;
  out.push_back(std::move(loc));
}

void collect_refs_in_type(Type *type, const ParsedModule &mod, const Sym &sym,
                          std::vector<LspLocation> &out) {
  if (type == nullptr)
    return;
  if (type_matches_struct(type, sym))
    push_location(out, mod, type->start, type->end);
  if (type_matches_enum(type, sym))
    push_location(out, mod, type->start, type->end);
  if (type->type == TYPE_PTR) {
    auto *ptr = std::get<type::Ptr *>(type->data);
    collect_refs_in_type(ptr->inner, mod, sym, out);
  }
  if (type->type == TYPE_UNION) {
    for (Type *member: std::get<type::Union *>(type->data)->members)
      collect_refs_in_type(member, mod, sym, out);
  }
  if (type->type == TYPE_TUPLE) {
    for (Type *element: std::get<type::Tuple *>(type->data)->elements)
      collect_refs_in_type(element, mod, sym, out);
  }
}

void collect_refs_in_expr(Expr *expr, const ParsedModule &mod, const Sym &sym,
                          std::string_view current_proc,
                          std::vector<LspLocation> &out) {
  if (expr == nullptr)
    return;

  switch (expr->type) {
  case EXPR_VAR: {
    auto *var = std::get<expr::Var *>(expr->data);
    if (sym.kind == SymKind::Const && var_matches_const(var, mod, sym))
      push_location(out, mod, var->var.start, var->var.end);
    if (sym.kind == SymKind::LocalVar && current_proc == sym.proc_name &&
        !var->module.has_value() && var->var.id_value == sym.name)
      push_location(out, mod, var->var.start, var->var.end);
    if (sym.kind == SymKind::Param && current_proc == sym.proc_name &&
        !var->module.has_value() && var->var.id_value == sym.name)
      push_location(out, mod, var->var.start, var->var.end);
    break;
  }
  case EXPR_CALL: {
    auto *call = std::get<expr::Call *>(expr->data);
    if (call_matches_proc(call, mod, sym) && call->name.has_value())
      push_location(out, mod, call->name->start, call->name->end);
    for (Expr *arg: call->arguments)
      collect_refs_in_expr(arg, mod, sym, current_proc, out);
    if (call->receiver != nullptr)
      collect_refs_in_expr(call->receiver, mod, sym, current_proc, out);
    return;
  }
  case EXPR_FIELD: {
    auto *field = std::get<expr::Field *>(expr->data);
    if (sym.kind == SymKind::Enum && field->field.id_value == sym.name)
      push_location(out, mod, field->field.start, field->field.end);
    if (sym.kind == SymKind::StructField &&
        field->field.id_value == sym.name &&
        field->base->expr_type != nullptr) {
      Type *struct_type = field->base->expr_type->as_struct_for_field_access();
      if (struct_type != nullptr &&
          std::get<type::Struct *>(struct_type->data)->name ==
              sym.struct_name)
        push_location(out, mod, field->field.start, field->field.end);
    }
    collect_refs_in_expr(field->base, mod, sym, current_proc, out);
    return;
  }
  case EXPR_ENUM_CASE: {
    auto *case_ = std::get<expr::EnumCase *>(expr->data);
    if (sym.kind == SymKind::Enum && case_->member.id_value == sym.name)
      push_location(out, mod, case_->member.start, case_->member.end);
    return;
  }
  case EXPR_STRUCT_LIT: {
    auto *lit = std::get<expr::StructLit *>(expr->data);
    if (sym.kind == SymKind::Struct && lit->type_name.id_value == sym.name)
      push_location(out, mod, lit->type_name.start, lit->type_name.end);
    for (const auto &f: lit->fields)
      collect_refs_in_expr(f.value, mod, sym, current_proc, out);
    return;
  }
  case EXPR_BINARY: {
    auto *bin = std::get<expr::Binary *>(expr->data);
    collect_refs_in_expr(bin->lhs, mod, sym, current_proc, out);
    collect_refs_in_expr(bin->rhs, mod, sym, current_proc, out);
    return;
  }
  case EXPR_GROUP:
    collect_refs_in_expr(std::get<expr::Group *>(expr->data)->expr, mod, sym,
                         current_proc, out);
    return;
  case EXPR_NOT:
    collect_refs_in_expr(std::get<expr::Not *>(expr->data)->expr, mod, sym,
                         current_proc, out);
    return;
  case EXPR_REF:
    collect_refs_in_expr(std::get<expr::Ref *>(expr->data)->expr, mod, sym,
                         current_proc, out);
    return;
  case EXPR_DEREF:
    collect_refs_in_expr(std::get<expr::Deref *>(expr->data)->expr, mod, sym,
                         current_proc, out);
    return;
  case EXPR_CAST: {
    auto *cast_ = std::get<expr::Cast *>(expr->data);
    collect_refs_in_type(cast_->target, mod, sym, out);
    collect_refs_in_expr(cast_->expr, mod, sym, current_proc, out);
    return;
  }
  case EXPR_INDEX: {
    auto *index = std::get<expr::Index *>(expr->data);
    collect_refs_in_expr(index->base, mod, sym, current_proc, out);
    collect_refs_in_expr(index->index, mod, sym, current_proc, out);
    return;
  }
  default:
    break;
  }
}

void collect_refs_in_stmts(const std::vector<Stmt *> &stmts,
                           const ParsedModule &mod, const Sym &sym,
                           std::string_view current_proc,
                           std::vector<LspLocation> &out) {
  for (Stmt *stmt: stmts) {
    switch (stmt->type) {
    case STMT_VAR: {
      auto *var = std::get<stmt::Var *>(stmt->data);
      if (sym.kind == SymKind::LocalVar && current_proc == sym.proc_name &&
          var->name.id_value == sym.name)
        push_location(out, mod, var->name.start, var->name.end);
      if (var->type != nullptr)
        collect_refs_in_type(var->type, mod, sym, out);
      if (var->value.has_value())
        collect_refs_in_expr(var->value.value(), mod, sym, current_proc, out);
      break;
    }
    case STMT_EXPR:
      collect_refs_in_expr(std::get<stmt::ExprStmt *>(stmt->data)->expr, mod,
                           sym, current_proc, out);
      break;
    case STMT_RETURN: {
      auto *ret = std::get<stmt::Return *>(stmt->data);
      if (ret->value.has_value())
        collect_refs_in_expr(ret->value.value(), mod, sym, current_proc, out);
      break;
    }
    case STMT_ASSIGN: {
      auto *assign = std::get<stmt::Assign *>(stmt->data);
      collect_refs_in_expr(assign->target, mod, sym, current_proc, out);
      collect_refs_in_expr(assign->value, mod, sym, current_proc, out);
      break;
    }
    case STMT_IF: {
      auto *iff = std::get<stmt::If *>(stmt->data);
      collect_refs_in_expr(iff->cond, mod, sym, current_proc, out);
      collect_refs_in_stmts(iff->then_block, mod, sym, current_proc, out);
      collect_refs_in_stmts(iff->else_block, mod, sym, current_proc, out);
      break;
    }
    case STMT_WHILE: {
      auto *loop = std::get<stmt::While *>(stmt->data);
      collect_refs_in_expr(loop->cond, mod, sym, current_proc, out);
      collect_refs_in_stmts(loop->body, mod, sym, current_proc, out);
      break;
    }
    case STMT_FOR: {
      auto *loop = std::get<stmt::For *>(stmt->data);
      if (loop->init != nullptr) {
        if (loop->init->type == STMT_VAR) {
          auto *var = std::get<stmt::Var *>(loop->init->data);
          if (var->type != nullptr)
            collect_refs_in_type(var->type, mod, sym, out);
          if (var->value.has_value())
        collect_refs_in_expr(var->value.value(), mod, sym, current_proc, out);
        } else if (loop->init->type == STMT_ASSIGN) {
          auto *assign = std::get<stmt::Assign *>(loop->init->data);
          collect_refs_in_expr(assign->target, mod, sym, current_proc, out);
          collect_refs_in_expr(assign->value, mod, sym, current_proc, out);
        }
      }
      collect_refs_in_expr(loop->cond, mod, sym, current_proc, out);
      if (loop->step != nullptr && loop->step->type == STMT_ASSIGN) {
        auto *assign = std::get<stmt::Assign *>(loop->step->data);
        collect_refs_in_expr(assign->target, mod, sym, current_proc, out);
        collect_refs_in_expr(assign->value, mod, sym, current_proc, out);
      }
      collect_refs_in_stmts(loop->body, mod, sym, current_proc, out);
      break;
    }
    case STMT_BLOCK:
      collect_refs_in_stmts(std::get<stmt::Block *>(stmt->data)->stmts, mod, sym,
                            current_proc, out);
      break;
    case STMT_WHEN: {
      auto *when = std::get<stmt::When *>(stmt->data);
      collect_refs_in_expr(when->cond, mod, sym, current_proc, out);
      collect_refs_in_stmts(when->true_block, mod, sym, current_proc, out);
      collect_refs_in_stmts(when->false_block, mod, sym, current_proc, out);
      break;
    }
    default:
      break;
    }
  }
}

std::vector<LspLocation> find_symbol_references(const Project &project,
                                                const Sym &sym,
                                                bool include_declaration) {
  std::vector<LspLocation> out{};

  auto add_def = [&]() {
    const ParsedModule *def_mod = module_for_path(project, sym.file_path);
    if (def_mod != nullptr)
      push_location(out, *def_mod, sym.def_start, sym.def_end);
  };

  if (sym.kind == SymKind::LocalVar || sym.kind == SymKind::Param) {
    const ParsedModule *mod = module_for_path(project, sym.file_path);
    if (mod == nullptr)
      return out;
    if (include_declaration)
      add_def();
    for (Decl *decl: mod->decls) {
      if (decl->type != DECL_PROC)
        continue;
      auto *proc = std::get<decl::Proc *>(decl->data);
      if (proc->name.id_value != sym.proc_name)
        continue;
      collect_refs_in_stmts(proc->body, *mod, sym, sym.proc_name, out);
    }
    return out;
  }

  for (const auto &[path, mod]: project.modules()) {
    for (Decl *decl: mod.decls) {
      switch (decl->type) {
      case DECL_PROC: {
        auto *proc = std::get<decl::Proc *>(decl->data);
        if (sym.kind == SymKind::Proc && mod.module_name == sym.module_name &&
            proc->name.id_value == sym.name)
          push_location(out, mod, proc->name.start, proc->name.end);
        for (const Param &param: proc->params)
          collect_refs_in_type(param.type, mod, sym, out);
        if (proc->ret_type.has_value())
          collect_refs_in_type(proc->ret_type.value(), mod, sym, out);
        collect_refs_in_stmts(proc->body, mod, sym, proc->name.id_value, out);
        break;
      }
      case DECL_STRUCT: {
        auto *strukt = std::get<decl::Struct *>(decl->data);
        if (sym.kind == SymKind::Struct && mod.module_name == sym.module_name &&
            strukt->name.id_value == sym.name)
          push_location(out, mod, strukt->name.start, strukt->name.end);
        for (const auto &field: strukt->fields) {
          if (sym.kind == SymKind::StructField &&
              mod.module_name == sym.module_name &&
              strukt->name.id_value == sym.struct_name &&
              field.name.id_value == sym.name)
            push_location(out, mod, field.name.start, field.name.end);
          collect_refs_in_type(field.type, mod, sym, out);
        }
        break;
      }
      case DECL_ENUM: {
        auto *enum_ = std::get<decl::Enum *>(decl->data);
        if (sym.kind == SymKind::Enum && mod.module_name == sym.module_name &&
            enum_->name.id_value == sym.name)
          push_location(out, mod, enum_->name.start, enum_->name.end);
        for (const auto &member: enum_->members) {
          if (sym.kind == SymKind::Enum && mod.module_name == sym.module_name &&
              member.name.id_value == sym.name)
            push_location(out, mod, member.name.start, member.name.end);
        }
        break;
      }
      case DECL_CONST: {
        auto *konst = std::get<decl::Const *>(decl->data);
        if (sym.kind == SymKind::Const && mod.module_name == sym.module_name &&
            konst->name.id_value == sym.name)
          push_location(out, mod, konst->name.start, konst->name.end);
        collect_refs_in_type(konst->type, mod, sym, out);
        collect_refs_in_expr(konst->value, mod, sym, "", out);
        break;
      }
      default:
        break;
      }
    }
  }

  if (include_declaration && sym.kind != SymKind::Proc &&
      sym.kind != SymKind::Struct && sym.kind != SymKind::Enum &&
      sym.kind != SymKind::Const && sym.kind != SymKind::StructField)
    add_def();

  return out;
}

struct RawSemanticToken {
  size_t start;
  size_t end;
  uint32_t type;
  uint32_t modifiers;
};

void add_sem_token(std::vector<RawSemanticToken> &tokens, size_t start,
                   size_t end, uint32_t type, uint32_t modifiers = 0) {
  if (start >= end)
    return;
  tokens.push_back({start, end, type, modifiers});
}

constexpr uint32_t SEM_TYPE = 0;
constexpr uint32_t SEM_STRUCT = 1;
constexpr uint32_t SEM_PARAMETER = 2;
constexpr uint32_t SEM_VARIABLE = 3;
constexpr uint32_t SEM_PROPERTY = 4;
constexpr uint32_t SEM_FUNCTION = 5;

constexpr uint32_t MOD_DECLARATION = 1 << 0;
constexpr uint32_t MOD_DEFINITION = 1 << 1;
constexpr uint32_t MOD_READONLY = 1 << 2;

void collect_sem_tokens_in_type(Type *type, std::vector<RawSemanticToken> &out) {
  if (type == nullptr)
    return;
  switch (type->type) {
  case TYPE_VOID:
  case TYPE_BOOL:
  case TYPE_INT:
  case TYPE_BYTE:
    add_sem_token(out, type->start, type->end, SEM_TYPE);
    break;
  case TYPE_STRUCT:
    add_sem_token(out, type->start, type->end, SEM_STRUCT);
    break;
  case TYPE_ENUM:
    add_sem_token(out, type->start, type->end, SEM_STRUCT);
    break;
  case TYPE_PTR:
    collect_sem_tokens_in_type(std::get<type::Ptr *>(type->data)->inner, out);
    break;
  case TYPE_TUPLE: {
    auto *tuple = std::get<type::Tuple *>(type->data);
    for (Type *element: tuple->elements)
      collect_sem_tokens_in_type(element, out);
    break;
  }
  case TYPE_UNION: {
    for (Type *member: std::get<type::Union *>(type->data)->members)
      collect_sem_tokens_in_type(member, out);
    break;
  }
  case TYPE_PROC: {
    auto *proc = std::get<type::Proc *>(type->data);
    for (const Param &param: proc->params) {
      add_sem_token(out, param.name.start, param.name.end, SEM_PARAMETER);
      collect_sem_tokens_in_type(param.type, out);
    }
    collect_sem_tokens_in_type(proc->ret_type, out);
    break;
  }
  }
}

void collect_sem_tokens_in_expr(Expr *expr, std::vector<RawSemanticToken> &out) {
  if (expr == nullptr)
    return;
  switch (expr->type) {
  case EXPR_VAR: {
    auto *var = std::get<expr::Var *>(expr->data);
    if (var->module.has_value())
      add_sem_token(out, var->module->start, var->module->end, SEM_VARIABLE);
    add_sem_token(out, var->var.start, var->var.end, SEM_VARIABLE);
    break;
  }
  case EXPR_CALL: {
    auto *call = std::get<expr::Call *>(expr->data);
    if (call->module.has_value())
      add_sem_token(out, call->module->start, call->module->end, SEM_VARIABLE);
    if (call->name.has_value())
      add_sem_token(out, call->name->start, call->name->end, SEM_FUNCTION);
    if (call->callee != nullptr)
      collect_sem_tokens_in_expr(call->callee, out);
    for (Expr *arg: call->arguments)
      collect_sem_tokens_in_expr(arg, out);
    if (call->receiver != nullptr)
      collect_sem_tokens_in_expr(call->receiver, out);
    return;
  }
  case EXPR_FIELD: {
    auto *field = std::get<expr::Field *>(expr->data);
    collect_sem_tokens_in_expr(field->base, out);
    add_sem_token(out, field->field.start, field->field.end, SEM_PROPERTY);
    return;
  }
  case EXPR_STRUCT_LIT: {
    auto *lit = std::get<expr::StructLit *>(expr->data);
    add_sem_token(out, lit->type_name.start, lit->type_name.end, SEM_STRUCT);
    for (const auto &f: lit->fields) {
      add_sem_token(out, f.name.start, f.name.end, SEM_PROPERTY);
      collect_sem_tokens_in_expr(f.value, out);
    }
    return;
  }
  case EXPR_STRING:
    break;
  case EXPR_INT:
  case EXPR_BOOL:
    break;
  case EXPR_BINARY: {
    auto *bin = std::get<expr::Binary *>(expr->data);
    collect_sem_tokens_in_expr(bin->lhs, out);
    collect_sem_tokens_in_expr(bin->rhs, out);
    return;
  }
  case EXPR_GROUP:
    collect_sem_tokens_in_expr(std::get<expr::Group *>(expr->data)->expr, out);
    return;
  case EXPR_NOT:
    collect_sem_tokens_in_expr(std::get<expr::Not *>(expr->data)->expr, out);
    return;
  case EXPR_REF:
    collect_sem_tokens_in_expr(std::get<expr::Ref *>(expr->data)->expr, out);
    return;
  case EXPR_DEREF:
    collect_sem_tokens_in_expr(std::get<expr::Deref *>(expr->data)->expr, out);
    return;
  case EXPR_CAST: {
    auto *cast_ = std::get<expr::Cast *>(expr->data);
    collect_sem_tokens_in_type(cast_->target, out);
    collect_sem_tokens_in_expr(cast_->expr, out);
    return;
  }
  case EXPR_SIZEOF:
    break;
  case EXPR_ENUM_CASE: {
    auto *case_ = std::get<expr::EnumCase *>(expr->data);
    add_sem_token(out, case_->member.start, case_->member.end, SEM_PROPERTY);
    return;
  }
  case EXPR_INDEX: {
    auto *index = std::get<expr::Index *>(expr->data);
    collect_sem_tokens_in_expr(index->base, out);
    collect_sem_tokens_in_expr(index->index, out);
    return;
  }
  default:
    break;
  }
}

void collect_sem_tokens_in_stmts(const std::vector<Stmt *> &stmts,
                                 std::vector<RawSemanticToken> &out) {
  for (Stmt *stmt: stmts) {
    switch (stmt->type) {
    case STMT_VAR: {
      auto *var = std::get<stmt::Var *>(stmt->data);
      add_sem_token(out, var->name.start, var->name.end, SEM_VARIABLE,
                    MOD_DECLARATION);
      if (var->type != nullptr)
        collect_sem_tokens_in_type(var->type, out);
      if (var->value.has_value())
        collect_sem_tokens_in_expr(var->value.value(), out);
      break;
    }
    case STMT_EXPR:
      collect_sem_tokens_in_expr(
          std::get<stmt::ExprStmt *>(stmt->data)->expr, out);
      break;
    case STMT_RETURN: {
      auto *ret = std::get<stmt::Return *>(stmt->data);
      if (ret->value.has_value())
        collect_sem_tokens_in_expr(ret->value.value(), out);
      break;
    }
    case STMT_IF: {
      auto *iff = std::get<stmt::If *>(stmt->data);
      collect_sem_tokens_in_expr(iff->cond, out);
      collect_sem_tokens_in_stmts(iff->then_block, out);
      collect_sem_tokens_in_stmts(iff->else_block, out);
      break;
    }
    case STMT_WHILE: {
      auto *loop = std::get<stmt::While *>(stmt->data);
      collect_sem_tokens_in_expr(loop->cond, out);
      collect_sem_tokens_in_stmts(loop->body, out);
      break;
    }
    case STMT_FOR: {
      auto *loop = std::get<stmt::For *>(stmt->data);
      if (loop->init != nullptr)
        collect_sem_tokens_in_stmts({loop->init}, out);
      collect_sem_tokens_in_expr(loop->cond, out);
      if (loop->step != nullptr)
        collect_sem_tokens_in_stmts({loop->step}, out);
      collect_sem_tokens_in_stmts(loop->body, out);
      break;
    }
    case STMT_BREAK:
    case STMT_CONTINUE:
      break;
    case STMT_ASSIGN: {
      auto *assign = std::get<stmt::Assign *>(stmt->data);
      collect_sem_tokens_in_expr(assign->target, out);
      collect_sem_tokens_in_expr(assign->value, out);
      break;
    }
    case STMT_BLOCK:
      collect_sem_tokens_in_stmts(std::get<stmt::Block *>(stmt->data)->stmts,
                                  out);
      break;
    case STMT_WHEN: {
      auto *when = std::get<stmt::When *>(stmt->data);
      collect_sem_tokens_in_expr(when->cond, out);
      collect_sem_tokens_in_stmts(when->true_block, out);
      collect_sem_tokens_in_stmts(when->false_block, out);
      break;
    }
    default:
      break;
    }
  }
}

SemanticTokens encode_semantic_tokens(std::string_view source,
                                      std::vector<RawSemanticToken> raw) {
  std::sort(raw.begin(), raw.end(),
            [](const RawSemanticToken &a, const RawSemanticToken &b) {
              return a.start < b.start;
            });

  std::vector<uint32_t> data{};
  size_t prev_line = 0;
  size_t prev_char = 0;

  for (const auto &tok: raw) {
    auto [line, character] = offset_to_line_column(source, tok.start);
    size_t length = tok.end - tok.start;

    uint32_t delta_line = 0;
    uint32_t delta_char = 0;
    if (data.empty()) {
      delta_line = static_cast<uint32_t>(line);
      delta_char = static_cast<uint32_t>(character);
    } else if (line == prev_line) {
      delta_char = static_cast<uint32_t>(character - prev_char);
    } else {
      delta_line = static_cast<uint32_t>(line - prev_line);
      delta_char = static_cast<uint32_t>(character);
    }

    data.push_back(delta_line);
    data.push_back(delta_char);
    data.push_back(static_cast<uint32_t>(length));
    data.push_back(tok.type);
    data.push_back(tok.modifiers);

    prev_line = line;
    prev_char = character;
  }

  return SemanticTokens{.data = std::move(data)};
}

std::optional<LspLocation> location_for_type(const Project &project, Type *type) {
  if (type == nullptr)
    return std::nullopt;
  if (type->type == TYPE_PTR)
    return location_for_type(project,
                             std::get<type::Ptr *>(type->data)->inner);
  if (type->type == TYPE_STRUCT) {
    std::string_view name = std::get<type::Struct *>(type->data)->name;
    for (const auto &[path, mod]: project.modules()) {
      if (Decl *decl = find_top_level_decl(const_cast<ParsedModule &>(mod), name,
                                           DECL_STRUCT)) {
        auto *strukt = std::get<decl::Struct *>(decl->data);
        return LspLocation{
            .uri = lsp_path_to_uri(mod.abs_path),
            .range = make_range(mod.source, strukt->name.start, strukt->name.end),
        };
      }
    }
  }
  if (type->type == TYPE_ENUM) {
    std::string_view name = std::get<type::Enum *>(type->data)->name;
    for (const auto &[path, mod]: project.modules()) {
      if (Decl *decl = find_top_level_decl(const_cast<ParsedModule &>(mod), name,
                                           DECL_ENUM)) {
        auto *enum_ = std::get<decl::Enum *>(decl->data);
        return LspLocation{
            .uri = lsp_path_to_uri(mod.abs_path),
            .range = make_range(mod.source, enum_->name.start, enum_->name.end),
        };
      }
    }
  }
  return std::nullopt;
}

std::optional<Type *> type_at_offset(const ParsedModule &module, size_t offset) {
  for (Decl *decl: module.decls) {
    if (decl->type == DECL_PROC) {
      auto *proc = std::get<decl::Proc *>(decl->data);
      for (const Param &param: proc->params) {
        if (offset_in_token(offset, param.type->start, param.type->end))
          return param.type;
      }
      if (proc->ret_type.has_value() &&
          offset_in_token(offset, proc->ret_type.value()->start,
                          proc->ret_type.value()->end))
        return proc->ret_type.value();
    }
    if (decl->type == DECL_STRUCT) {
      auto *strukt = std::get<decl::Struct *>(decl->data);
      for (const auto &field: strukt->fields) {
        if (offset_in_token(offset, field.type->start, field.type->end))
          return field.type;
      }
    }
    if (decl->type == DECL_CONST) {
      auto *konst = std::get<decl::Const *>(decl->data);
      if (offset_in_token(offset, konst->type->start, konst->type->end))
        return konst->type;
    }
  }

  auto enclosing = proc_containing_offset(module, offset);
  if (enclosing.has_value()) {
    for (Decl *decl: module.decls) {
      if (decl->type != DECL_PROC)
        continue;
      auto *proc = std::get<decl::Proc *>(decl->data);
      if (proc->name.id_value != enclosing.value())
        continue;
      for (Stmt *stmt: proc->body) {
        if (stmt->type == STMT_VAR) {
          auto *var = std::get<stmt::Var *>(stmt->data);
          if (var->type != nullptr &&
              offset_in_token(offset, var->type->start, var->type->end))
            return var->type;
        }
      }
    }
  }

  Expr *expr = find_expr_in_module(module, offset);
  if (expr != nullptr && expr->expr_type != nullptr)
    return expr->expr_type;

  return std::nullopt;
}

void collect_folding_in_stmts(const ParsedModule &mod,
                              const std::vector<Stmt *> &stmts,
                              std::vector<FoldingRange> &out) {
  for (Stmt *stmt: stmts) {
    auto [start_line, _] = offset_to_line_column(mod.source, stmt->start);
    auto [end_line, __] = offset_to_line_column(mod.source, stmt->end);
    switch (stmt->type) {
    case STMT_BLOCK:
    case STMT_IF:
    case STMT_WHILE:
    case STMT_FOR:
    case STMT_COMPTIME_BLOCK:
      if (end_line > start_line)
        out.push_back(FoldingRange{.start_line = start_line,
                                   .end_line = end_line,
                                   .kind = 0});
      break;
    default:
      break;
    }

    switch (stmt->type) {
    case STMT_IF: {
      auto *iff = std::get<stmt::If *>(stmt->data);
      collect_folding_in_stmts(mod, iff->then_block, out);
      collect_folding_in_stmts(mod, iff->else_block, out);
      break;
    }
    case STMT_WHILE:
      collect_folding_in_stmts(
          mod, std::get<stmt::While *>(stmt->data)->body, out);
      break;
    case STMT_FOR: {
      auto *loop = std::get<stmt::For *>(stmt->data);
      if (loop->init != nullptr)
        collect_folding_in_stmts(mod, {loop->init}, out);
      collect_folding_in_stmts(mod, loop->body, out);
      break;
    }
    case STMT_BLOCK:
      collect_folding_in_stmts(mod,
                               std::get<stmt::Block *>(stmt->data)->stmts, out);
      break;
    case STMT_WHEN: {
      auto *when = std::get<stmt::When *>(stmt->data);
      collect_folding_in_stmts(mod, when->true_block, out);
      collect_folding_in_stmts(mod, when->false_block, out);
      break;
    }
    default:
      break;
    }
  }
}

void collect_calls_in_expr(const Project &project, Expr *expr,
                           const ParsedModule &mod, std::string_view caller_proc,
                           std::string_view callee_module,
                           std::string_view callee_name,
                           std::vector<CallHierarchyCall> &out) {
  if (expr == nullptr)
    return;
  if (expr->type == EXPR_CALL) {
    auto *call = std::get<expr::Call *>(expr->data);
    if (call->name.has_value()) {
      std::string_view use_module = mod.module_name;
      if (call->module.has_value())
        use_module = call->module->id_value;
      else if (call->resolved_module.has_value())
        use_module = *call->resolved_module;
      if (use_module == callee_module && call->name->id_value == callee_name &&
          call->name.has_value()) {
        CallHierarchyItem target{};
        if (auto proc = project.registry().find_proc(callee_module, callee_name);
            proc.has_value()) {
          for (const auto &[path, m]: project.modules()) {
            if (m.module_name != callee_module)
              continue;
            for (Decl *decl: m.decls) {
              if (decl->type != DECL_PROC)
                continue;
              auto *p = std::get<decl::Proc *>(decl->data);
              if (p->name.id_value != callee_name)
                continue;
              target = CallHierarchyItem{
                  .name = std::string(callee_name),
                  .detail = proc_signature(*proc),
                  .uri = lsp_path_to_uri(m.abs_path),
                  .range = make_range(m.source, p->name.start, p->name.end),
                  .selection_range =
                      make_range(m.source, p->name.start, p->name.end),
                  .data = std::format("{}:{}", callee_module, callee_name),
              };
            }
          }
        }
        if (!target.uri.empty()) {
          out.push_back(CallHierarchyCall{
              .range = make_range(mod.source, call->name->start, call->name->end),
              .to = std::move(target),
          });
        }
      }
    }
    for (Expr *arg: call->arguments)
      collect_calls_in_expr(project, arg, mod, caller_proc, callee_module,
                            callee_name, out);
    if (call->receiver != nullptr)
      collect_calls_in_expr(project, call->receiver, mod, caller_proc,
                            callee_module, callee_name, out);
    return;
  }
  switch (expr->type) {
  case EXPR_BINARY: {
    auto *bin = std::get<expr::Binary *>(expr->data);
    collect_calls_in_expr(project, bin->lhs, mod, caller_proc, callee_module,
                          callee_name, out);
    collect_calls_in_expr(project, bin->rhs, mod, caller_proc, callee_module,
                          callee_name, out);
    break;
  }
  case EXPR_GROUP:
    collect_calls_in_expr(project, std::get<expr::Group *>(expr->data)->expr,
                          mod, caller_proc, callee_module, callee_name, out);
    break;
  case EXPR_NOT:
    collect_calls_in_expr(project, std::get<expr::Not *>(expr->data)->expr, mod,
                          caller_proc, callee_module, callee_name, out);
    break;
  case EXPR_REF:
    collect_calls_in_expr(project, std::get<expr::Ref *>(expr->data)->expr,
                          mod, caller_proc, callee_module, callee_name, out);
    break;
  case EXPR_DEREF:
    collect_calls_in_expr(project, std::get<expr::Deref *>(expr->data)->expr,
                          mod, caller_proc, callee_module, callee_name, out);
    break;
  case EXPR_FIELD:
    collect_calls_in_expr(project,
                          std::get<expr::Field *>(expr->data)->base, mod,
                          caller_proc, callee_module, callee_name, out);
    break;
  case EXPR_INDEX: {
    auto *index = std::get<expr::Index *>(expr->data);
    collect_calls_in_expr(project, index->base, mod, caller_proc, callee_module,
                          callee_name, out);
    collect_calls_in_expr(project, index->index, mod, caller_proc,
                          callee_module, callee_name, out);
    break;
  }
  case EXPR_CAST:
    collect_calls_in_expr(project, std::get<expr::Cast *>(expr->data)->expr,
                          mod, caller_proc, callee_module, callee_name, out);
    break;
  case EXPR_STRUCT_LIT: {
    auto *lit = std::get<expr::StructLit *>(expr->data);
    for (const auto &f: lit->fields)
      collect_calls_in_expr(project, f.value, mod, caller_proc, callee_module,
                            callee_name, out);
    break;
  }
  default:
    break;
  }
}

void collect_calls_in_stmts(const Project &project, const ParsedModule &mod,
                            const std::vector<Stmt *> &stmts,
                            std::string_view caller_proc,
                            std::string_view callee_module,
                            std::string_view callee_name,
                            std::vector<CallHierarchyCall> &out) {
  for (Stmt *stmt: stmts) {
    switch (stmt->type) {
    case STMT_EXPR:
      collect_calls_in_expr(
          project, std::get<stmt::ExprStmt *>(stmt->data)->expr, mod,
          caller_proc, callee_module, callee_name, out);
      break;
    case STMT_VAR: {
      auto *var = std::get<stmt::Var *>(stmt->data);
      if (var->value.has_value())
        collect_calls_in_expr(project, var->value.value(), mod, caller_proc,
                              callee_module, callee_name, out);
      break;
    }
    case STMT_RETURN: {
      auto *ret = std::get<stmt::Return *>(stmt->data);
      if (ret->value.has_value())
        collect_calls_in_expr(project, ret->value.value(), mod, caller_proc,
                              callee_module, callee_name, out);
      break;
    }
    case STMT_ASSIGN:
      collect_calls_in_expr(project,
                            std::get<stmt::Assign *>(stmt->data)->value, mod,
                            caller_proc, callee_module, callee_name, out);
      break;
    case STMT_IF: {
      auto *iff = std::get<stmt::If *>(stmt->data);
      collect_calls_in_expr(project, iff->cond, mod, caller_proc, callee_module,
                            callee_name, out);
      collect_calls_in_stmts(project, mod, iff->then_block, caller_proc,
                             callee_module, callee_name, out);
      collect_calls_in_stmts(project, mod, iff->else_block, caller_proc,
                             callee_module, callee_name, out);
      break;
    }
    case STMT_WHILE: {
      auto *loop = std::get<stmt::While *>(stmt->data);
      collect_calls_in_expr(project, loop->cond, mod, caller_proc, callee_module,
                            callee_name, out);
      collect_calls_in_stmts(project, mod, loop->body, caller_proc,
                             callee_module, callee_name, out);
      break;
    }
    case STMT_FOR: {
      auto *loop = std::get<stmt::For *>(stmt->data);
      if (loop->init != nullptr)
        collect_calls_in_stmts(project, mod, {loop->init}, caller_proc,
                             callee_module, callee_name, out);
      collect_calls_in_expr(project, loop->cond, mod, caller_proc, callee_module,
                            callee_name, out);
      if (loop->step != nullptr)
        collect_calls_in_stmts(project, mod, {loop->step}, caller_proc,
                             callee_module, callee_name, out);
      collect_calls_in_stmts(project, mod, loop->body, caller_proc,
                             callee_module, callee_name, out);
      break;
    }
    case STMT_BLOCK:
      collect_calls_in_stmts(
          project, mod, std::get<stmt::Block *>(stmt->data)->stmts, caller_proc,
          callee_module, callee_name, out);
      break;
    case STMT_WHEN: {
      auto *when = std::get<stmt::When *>(stmt->data);
      collect_calls_in_expr(project, when->cond, mod, caller_proc, callee_module,
                            callee_name, out);
      collect_calls_in_stmts(project, mod, when->true_block, caller_proc,
                             callee_module, callee_name, out);
      collect_calls_in_stmts(project, mod, when->false_block, caller_proc,
                             callee_module, callee_name, out);
      break;
    }
    default:
      break;
    }
  }
}

void collect_outgoing_calls_expr(const Project &project, const ParsedModule &mod,
                                 Expr *expr,
                                 std::vector<CallHierarchyCall> &out,
                                 std::set<std::string> &seen) {
  if (expr == nullptr)
    return;
  if (expr->type == EXPR_CALL) {
    auto *call = std::get<expr::Call *>(expr->data);
    if (call->name.has_value()) {
      std::string_view use_module = mod.module_name;
      if (call->module.has_value())
        use_module = call->module->id_value;
      else if (call->resolved_module.has_value())
        use_module = *call->resolved_module;
      std::string key =
          std::format("{}:{}", use_module, call->name->id_value);
      if (!seen.contains(key)) {
        if (auto proc =
                project.registry().find_proc(use_module, call->name->id_value);
            proc.has_value()) {
          CallHierarchyItem target{};
          for (const auto &[path, m]: project.modules()) {
            if (m.module_name != use_module)
              continue;
            for (Decl *d: m.decls) {
              if (d->type != DECL_PROC)
                continue;
              auto *p = std::get<decl::Proc *>(d->data);
              if (p->name.id_value != call->name->id_value)
                continue;
              target = CallHierarchyItem{
                  .name = std::string(call->name->id_value),
                  .detail = proc_signature(*proc),
                  .uri = lsp_path_to_uri(m.abs_path),
                  .range = make_range(m.source, p->name.start, p->name.end),
                  .selection_range =
                      make_range(m.source, p->name.start, p->name.end),
                  .data = key,
              };
            }
          }
          if (!target.uri.empty()) {
            seen.insert(key);
            out.push_back(CallHierarchyCall{
                .range =
                    make_range(mod.source, call->name->start, call->name->end),
                .to = std::move(target),
            });
          }
        }
      }
    }
    for (Expr *arg: call->arguments)
      collect_outgoing_calls_expr(project, mod, arg, out, seen);
    if (call->receiver != nullptr)
      collect_outgoing_calls_expr(project, mod, call->receiver, out, seen);
    return;
  }
  switch (expr->type) {
  case EXPR_BINARY: {
    auto *bin = std::get<expr::Binary *>(expr->data);
    collect_outgoing_calls_expr(project, mod, bin->lhs, out, seen);
    collect_outgoing_calls_expr(project, mod, bin->rhs, out, seen);
    break;
  }
  case EXPR_GROUP:
    collect_outgoing_calls_expr(project, mod,
                                std::get<expr::Group *>(expr->data)->expr, out,
                                seen);
    break;
  case EXPR_NOT:
    collect_outgoing_calls_expr(project, mod,
                                std::get<expr::Not *>(expr->data)->expr, out,
                                seen);
    break;
  case EXPR_REF:
    collect_outgoing_calls_expr(project, mod,
                                std::get<expr::Ref *>(expr->data)->expr, out,
                                seen);
    break;
  case EXPR_DEREF:
    collect_outgoing_calls_expr(project, mod,
                                std::get<expr::Deref *>(expr->data)->expr, out,
                                seen);
    break;
  case EXPR_FIELD:
    collect_outgoing_calls_expr(project, mod,
                                std::get<expr::Field *>(expr->data)->base, out,
                                seen);
    break;
  case EXPR_INDEX: {
    auto *index = std::get<expr::Index *>(expr->data);
    collect_outgoing_calls_expr(project, mod, index->base, out, seen);
    collect_outgoing_calls_expr(project, mod, index->index, out, seen);
    break;
  }
  case EXPR_CAST:
    collect_outgoing_calls_expr(project, mod,
                                std::get<expr::Cast *>(expr->data)->expr, out,
                                seen);
    break;
  case EXPR_STRUCT_LIT: {
    auto *lit = std::get<expr::StructLit *>(expr->data);
    for (const auto &f: lit->fields)
      collect_outgoing_calls_expr(project, mod, f.value, out, seen);
    break;
  }
  default:
    break;
  }
}

void collect_outgoing_calls_in_stmts(const Project &project,
                                     const ParsedModule &mod,
                                     const std::vector<Stmt *> &stmts,
                                     std::vector<CallHierarchyCall> &out,
                                     std::set<std::string> &seen) {
  for (Stmt *stmt: stmts) {
    switch (stmt->type) {
    case STMT_EXPR:
      collect_outgoing_calls_expr(
          project, mod, std::get<stmt::ExprStmt *>(stmt->data)->expr, out,
          seen);
      break;
    case STMT_VAR: {
      auto *var = std::get<stmt::Var *>(stmt->data);
      if (var->value.has_value())
        collect_outgoing_calls_expr(project, mod, var->value.value(), out,
                                  seen);
      break;
    }
    case STMT_RETURN: {
      auto *ret = std::get<stmt::Return *>(stmt->data);
      if (ret->value.has_value())
        collect_outgoing_calls_expr(project, mod, ret->value.value(), out,
                                  seen);
      break;
    }
    case STMT_ASSIGN:
      collect_outgoing_calls_expr(
          project, mod, std::get<stmt::Assign *>(stmt->data)->value, out, seen);
      break;
    case STMT_IF: {
      auto *iff = std::get<stmt::If *>(stmt->data);
      collect_outgoing_calls_expr(project, mod, iff->cond, out, seen);
      collect_outgoing_calls_in_stmts(project, mod, iff->then_block, out, seen);
      collect_outgoing_calls_in_stmts(project, mod, iff->else_block, out, seen);
      break;
    }
    case STMT_WHILE: {
      auto *loop = std::get<stmt::While *>(stmt->data);
      collect_outgoing_calls_expr(project, mod, loop->cond, out, seen);
      collect_outgoing_calls_in_stmts(project, mod, loop->body, out, seen);
      break;
    }
    case STMT_FOR: {
      auto *loop = std::get<stmt::For *>(stmt->data);
      if (loop->init != nullptr)
        collect_outgoing_calls_in_stmts(project, mod, {loop->init}, out, seen);
      collect_outgoing_calls_expr(project, mod, loop->cond, out, seen);
      if (loop->step != nullptr)
        collect_outgoing_calls_in_stmts(project, mod, {loop->step}, out, seen);
      collect_outgoing_calls_in_stmts(project, mod, loop->body, out, seen);
      break;
    }
    case STMT_BLOCK:
      collect_outgoing_calls_in_stmts(
          project, mod, std::get<stmt::Block *>(stmt->data)->stmts, out, seen);
      break;
    case STMT_WHEN: {
      auto *when = std::get<stmt::When *>(stmt->data);
      collect_outgoing_calls_expr(project, mod, when->cond, out, seen);
      collect_outgoing_calls_in_stmts(project, mod, when->true_block, out,
                                    seen);
      collect_outgoing_calls_in_stmts(project, mod, when->false_block, out,
                                    seen);
      break;
    }
    default:
      break;
    }
  }
}

void collect_incoming_calls(const Project &project, std::string_view callee_module,
                            std::string_view callee_name,
                            std::vector<CallHierarchyCall> &out) {
  for (const auto &[path, mod]: project.modules()) {
    for (Decl *decl: mod.decls) {
      if (decl->type != DECL_PROC)
        continue;
      auto *proc = std::get<decl::Proc *>(decl->data);
      std::vector<CallHierarchyCall> hits{};
      collect_calls_in_stmts(project, mod, proc->body, proc->name.id_value,
                             callee_module, callee_name, hits);
      for (auto &hit: hits) {
        CallHierarchyItem from{
            .name = std::string(proc->name.id_value),
            .detail = std::string(mod.module_name),
            .uri = lsp_path_to_uri(mod.abs_path),
            .range = make_range(mod.source, proc->name.start, proc->name.end),
            .selection_range =
                make_range(mod.source, proc->name.start, proc->name.end),
            .data = std::format("{}:{}", mod.module_name, proc->name.id_value),
        };
        out.push_back(CallHierarchyCall{.range = hit.range, .to = std::move(from)});
      }
    }
  }
}

bool symbol_name_matches(std::string_view name, std::string_view query) {
  if (query.empty())
    return true;
  std::string lower_name(name);
  std::string lower_query(query);
  for (char &c: lower_name)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  for (char &c: lower_query)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return lower_name.contains(lower_query);
}

} // namespace

std::string lsp_path_to_uri(std::string_view path) {
  std::string uri = "file://";
  if (!path.empty() && path[0] == '/')
    uri += path;
  else
    uri += "/" + std::string(path);
  return uri;
}

std::string lsp_uri_to_path(std::string_view uri) {
  if (uri.starts_with("file://"))
    return std::string(uri.substr(7));
  return std::string(uri);
}

LspRange lsp_range_from_offsets(std::string_view source, size_t start,
                                size_t end) {
  return make_range(source, start, end);
}

IdeService::IdeService(Project &project) : _project(project) {}

std::vector<DocumentSymbol>
IdeService::document_symbols(std::string_view abs_path) const {
  const ParsedModule *module = module_for_path(_project, abs_path);
  if (module == nullptr)
    return {};

  std::vector<DocumentSymbol> symbols{};
  for (Decl *decl: module->decls) {
    switch (decl->type) {
    case DECL_PROC: {
      auto *proc = std::get<decl::Proc *>(decl->data);
      symbols.push_back(DocumentSymbol{
          .name = std::string(module->source.substr(proc->name.start,
                                                    proc->name.end -
                                                        proc->name.start)),
          .kind = 12,
          .range = make_range(module->source, proc->name.start, proc->name.end),
      });
      break;
    }
    case DECL_STRUCT: {
      auto *strukt = std::get<decl::Struct *>(decl->data);
      DocumentSymbol symbol{
          .name = std::string(module->source.substr(strukt->name.start,
                                                    strukt->name.end -
                                                        strukt->name.start)),
          .kind = 22,
          .range = make_range(module->source, strukt->name.start,
                              strukt->name.end),
      };
      for (const auto &field: strukt->fields) {
        symbol.children.push_back(DocumentSymbol{
            .name = std::string(module->source.substr(
                field.name.start, field.name.end - field.name.start)),
            .kind = 8,
            .range = make_range(module->source, field.name.start,
                                field.name.end),
        });
      }
      symbols.push_back(std::move(symbol));
      break;
    }
    case DECL_ENUM: {
      auto *enum_ = std::get<decl::Enum *>(decl->data);
      DocumentSymbol symbol{
          .name = std::string(module->source.substr(enum_->name.start,
                                                    enum_->name.end -
                                                        enum_->name.start)),
          .kind = 10,
          .range = make_range(module->source, enum_->name.start,
                              enum_->name.end),
      };
      for (const auto &member: enum_->members) {
        symbol.children.push_back(DocumentSymbol{
            .name = std::string(module->source.substr(
                member.name.start, member.name.end - member.name.start)),
            .kind = 22,
            .range = make_range(module->source, member.name.start,
                                member.name.end),
        });
      }
      symbols.push_back(std::move(symbol));
      break;
    }
    case DECL_CONST: {
      auto *konst = std::get<decl::Const *>(decl->data);
      symbols.push_back(DocumentSymbol{
          .name = std::string(module->source.substr(konst->name.start,
                                                    konst->name.end -
                                                        konst->name.start)),
          .kind = 14,
          .range = make_range(module->source, konst->name.start, konst->name.end),
      });
      break;
    }
    default:
      break;
    }
  }
  return symbols;
}

std::optional<HoverInfo> IdeService::hover(std::string_view abs_path,
                                           size_t line,
                                           size_t character) const {
  const ParsedModule *module = module_for_path(_project, abs_path);
  if (module == nullptr)
    return std::nullopt;

  size_t offset = position_to_offset(module->source, line, character);
  auto word = word_at(*module, line, character);
  if (!word.has_value())
    return std::nullopt;

  for (Decl *decl: module->decls) {
    if (decl->type == DECL_PROC) {
      auto *proc = std::get<decl::Proc *>(decl->data);
      if (proc->name.id_value == word.value()) {
        for (const auto &checked: _project.registry().modules()) {
          if (checked.second.path != module->rel_path)
            continue;
          for (const auto &p: checked.second.procs) {
            if (p.name == word.value())
              return HoverInfo{.text = proc_signature(p)};
          }
        }
      }
    }
    if (decl->type == DECL_STRUCT) {
      auto *strukt = std::get<decl::Struct *>(decl->data);
      if (strukt->name.id_value == word.value())
        return HoverInfo{.text = std::format("struct {}", strukt->name.id_value)};
    }
    if (decl->type == DECL_ENUM) {
      auto *enum_ = std::get<decl::Enum *>(decl->data);
      if (enum_->name.id_value == word.value())
        return HoverInfo{.text = std::format("enum {}", enum_->name.id_value)};
    }
    if (decl->type == DECL_CONST) {
      auto *konst = std::get<decl::Const *>(decl->data);
      if (konst->name.id_value == word.value()) {
        return HoverInfo{
            .text = std::format("const {} = ...", konst->name.id_value)};
      }
    }
  }

  if (Expr *call = find_call_in_module(*module, offset);
      call != nullptr && call->type == EXPR_CALL) {
    auto *call_expr = std::get<expr::Call *>(call->data);
    if (!call_expr->name.has_value())
      return std::nullopt;

    std::string_view module_name = module->module_name;
    if (call_expr->module.has_value())
      module_name = call_expr->module->id_value;
    else if (call_expr->resolved_module.has_value())
      module_name = *call_expr->resolved_module;

    auto proc =
        _project.registry().find_proc(module_name, call_expr->name->id_value);
    if (proc.has_value())
      return HoverInfo{.text = proc_signature(*proc)};
  }

  Expr *expr = find_expr_in_module(*module, offset);
  if (expr != nullptr && expr->expr_type != nullptr)
    return HoverInfo{.text = type_to_string(expr->expr_type)};

  for (const auto &entry: _project.registry().modules()) {
    for (const auto &proc: entry.second.procs) {
      if (proc.name == word.value())
        return HoverInfo{.text = proc_signature(proc)};
    }
    for (const auto &strukt: entry.second.structs) {
      if (strukt.name == word.value())
        return HoverInfo{.text = std::format("struct {}", strukt.name)};
    }
    for (const auto &enum_: entry.second.enums) {
      if (enum_.name == word.value())
        return HoverInfo{.text = std::format("enum {}", enum_.name)};
    }
    for (const auto &konst: entry.second.consts) {
      if (konst.name == word.value())
        return HoverInfo{
            .text = std::format("const {}: {}", konst.name,
                                type_to_string(konst.type))};
    }
  }

  return std::nullopt;
}

std::optional<LspLocation> IdeService::definition(std::string_view abs_path,
                                                  size_t line,
                                                  size_t character) const {
  auto sym = resolve_symbol(_project, abs_path, line, character);
  if (!sym.has_value())
    return std::nullopt;

  const ParsedModule *module = module_for_path(_project, sym->file_path);
  if (module == nullptr)
    return std::nullopt;

  return LspLocation{
      .uri = lsp_path_to_uri(sym->file_path),
      .range = make_range(module->source, sym->def_start, sym->def_end),
  };
}

std::vector<CompletionItem>
IdeService::completion(std::string_view abs_path, size_t line,
                       size_t character) const {
  const ParsedModule *module = module_for_path(_project, abs_path);
  if (module == nullptr)
    return {};

  size_t offset = position_to_offset(module->source, line, character);
  std::string prefix;
  if (auto span = identifier_span_at(module->source, offset); span.has_value())
    prefix = std::string(
        module->source.substr(span->first, span->second - span->first));

  std::vector<CompletionItem> items{};
  auto add_unique = [&](CompletionItem item) {
    if (std::any_of(items.begin(), items.end(),
                    [&](const CompletionItem &existing) {
                      return existing.label == item.label;
                    }))
      return;
    items.push_back(std::move(item));
  };

  size_t line_start = offset;
  while (line_start > 0 && module->source[line_start - 1] != '\n')
    line_start--;
  std::string line_prefix_str =
      std::string(module->source.substr(line_start, offset - line_start));
  if (line_prefix_str.find("import") != std::string::npos) {
    size_t quote = module->source.rfind('"', offset);
    if (quote != std::string::npos && quote >= line_start) {
      std::string partial =
          std::string(module->source.substr(quote + 1, offset - quote - 1));
      for (const auto &[p, mod]: _project.modules()) {
        std::string path = mod.rel_path;
        if (!partial.empty() && !path.starts_with(partial))
          continue;
        add_unique(CompletionItem{
            .label = path,
            .detail = "import",
            .insert_text = path,
            .kind = 9,
        });
      }
      return items;
    }
  }

  if (is_after_dot(module->source, offset)) {
    Expr *expr = find_expr_in_module(*module, offset);
    if (expr != nullptr) {
      if (expr->type == EXPR_FIELD) {
        auto *field = std::get<expr::Field *>(expr->data);
        if (field->base->type == EXPR_VAR) {
          auto *var = std::get<expr::Var *>(field->base->data);
          if (!var->module.has_value()) {
            if (auto enum_ = find_registry_enum(_project.registry(),
                                                module->module_name,
                                                var->var.id_value);
                enum_.has_value()) {
              add_enum_member_completions(_project.registry(), module->module_name,
                                          enum_->name, prefix, items, add_unique);
            }
          }
        }
        if (field->base->expr_type != nullptr &&
            field->base->expr_type->type == TYPE_ENUM) {
          std::string_view enum_name =
              std::get<type::Enum *>(field->base->expr_type->data)->name;
          add_enum_member_completions(_project.registry(), module->module_name,
                                      enum_name, prefix, items, add_unique);
        }
        Type *struct_type =
            field->base->expr_type != nullptr
                ? field->base->expr_type->as_struct_for_field_access()
                : nullptr;
        if (struct_type != nullptr) {
          std::string_view struct_name =
              std::get<type::Struct *>(struct_type->data)->name;
          auto strukt = _project.registry().find_struct(module->module_name,
                                                        struct_name);
          if (!strukt.has_value()) {
            for (const auto &entry: _project.registry().modules()) {
              strukt = _project.registry().find_struct(entry.first, struct_name);
              if (strukt.has_value())
                break;
            }
          }
          if (strukt.has_value()) {
            for (const auto &f: strukt->fields) {
              if (!prefix.empty() && !std::string(f.name).starts_with(prefix))
                continue;
              add_unique(CompletionItem{
                  .label = std::string(f.name),
                  .detail = type_to_string(f.type),
                  .insert_text = std::string(f.name),
                  .kind = 5,
              });
            }
          }
        }
      } else if (expr->type == EXPR_ENUM_CASE) {
        if (Expr *root = find_stmt_expr_root_in_module(*module, offset);
            root != nullptr) {
          if (Type *enum_type =
                  enum_type_for_enum_case_in_tree(_project.registry(),
                                                  module->module_name, root,
                                                  expr);
              enum_type != nullptr && enum_type->type == TYPE_ENUM) {
            std::string_view enum_name =
                std::get<type::Enum *>(enum_type->data)->name;
            add_enum_member_completions(_project.registry(), module->module_name,
                                        enum_name, prefix, items, add_unique);
          }
        }
      }
    }
    return items;
  }

  static const char *keywords[] = {
      "proc",  "var",   "const",  "struct", "enum", "union", "import",
      "extern", "return", "if",    "else",  "for",    "while", "break",
      "continue", "when",  "comptime", "sizeof", "cast", "true", "false",
  };
  for (const char *kw: keywords) {
    if (!prefix.empty() && !std::string_view(kw).starts_with(prefix))
      continue;
    add_unique(CompletionItem{
        .label = kw,
        .detail = "keyword",
        .insert_text = kw,
        .kind = 14,
    });
  }

  static const char *primitives[] = {"void", "bool", "int", "byte"};
  for (const char *ty: primitives) {
    if (!prefix.empty() && !std::string_view(ty).starts_with(prefix))
      continue;
    add_unique(CompletionItem{
        .label = ty,
        .detail = "type",
        .insert_text = ty,
        .kind = 7,
    });
  }

  for (const auto &entry: _project.registry().modules()) {
    for (const auto &proc: entry.second.procs) {
      std::string label = std::string(proc.name);
      if (!prefix.empty() && !label.starts_with(prefix))
        continue;
      add_unique(CompletionItem{
          .label = label,
          .detail = proc_signature(proc),
          .insert_text = label,
          .kind = 3,
      });
    }
    for (const auto &strukt: entry.second.structs) {
      std::string label = std::string(strukt.name);
      if (!prefix.empty() && !label.starts_with(prefix))
        continue;
      add_unique(CompletionItem{
          .label = label,
          .detail = "struct",
          .insert_text = label,
          .kind = 7,
      });
    }
    for (const auto &enum_: entry.second.enums) {
      std::string label = std::string(enum_.name);
      if (!prefix.empty() && !label.starts_with(prefix))
        continue;
      add_unique(CompletionItem{
          .label = label,
          .detail = "enum",
          .insert_text = label,
          .kind = 7,
      });
    }
    for (const auto &konst: entry.second.consts) {
      std::string label = std::string(konst.name);
      if (!prefix.empty() && !label.starts_with(prefix))
        continue;
      add_unique(CompletionItem{
          .label = label,
          .detail = type_to_string(konst.type),
          .insert_text = label,
          .kind = 21,
      });
    }
  }

  auto current_proc = proc_containing_offset(*module, offset);
  if (current_proc.has_value()) {
    for (Decl *decl: module->decls) {
      if (decl->type != DECL_PROC)
        continue;
      auto *proc = std::get<decl::Proc *>(decl->data);
      if (proc->name.id_value != current_proc.value())
        continue;

      for (const Param &param: proc->params) {
        std::string label = std::string(param.name.id_value);
        if (!prefix.empty() && !label.starts_with(prefix))
          continue;
        add_unique(CompletionItem{
            .label = label,
            .detail = type_to_string(param.type),
            .insert_text = label,
            .kind = 6,
        });
      }

      for (Stmt *stmt: proc->body) {
        if (stmt->type != STMT_VAR)
          continue;
        auto *var = std::get<stmt::Var *>(stmt->data);
        std::string label = std::string(var->name.id_value);
        if (!prefix.empty() && !label.starts_with(prefix))
          continue;
        add_unique(CompletionItem{
            .label = label,
            .detail = var->type != nullptr ? type_to_string(var->type) : "",
            .insert_text = label,
            .kind = 6,
        });
      }
      break;
    }
  }

  return items;
}

std::optional<SignatureInfo>
IdeService::signature_help(std::string_view abs_path, size_t line,
                           size_t character) const {
  const ParsedModule *module = module_for_path(_project, abs_path);
  if (module == nullptr)
    return std::nullopt;

  size_t offset = position_to_offset(module->source, line, character);
  Expr *call_expr = find_call_in_module(*module, offset);
  if (call_expr == nullptr || call_expr->type != EXPR_CALL)
    return std::nullopt;

  auto *call = std::get<expr::Call *>(call_expr->data);
  if (!call->name.has_value())
    return std::nullopt;

  std::string_view module_name = module->module_name;
  if (call->module.has_value())
    module_name = call->module->id_value;
  else if (call->resolved_module.has_value())
    module_name = *call->resolved_module;

  auto proc = _project.registry().find_proc(module_name, call->name->id_value);
  if (!proc.has_value())
    return std::nullopt;

  size_t arg_index = 0;
  if (call->receiver != nullptr)
    arg_index++;

  size_t comma_count = 0;
  size_t paren = call->name->end;
  while (paren < module->source.size() && module->source[paren] != '(')
    paren++;
  for (size_t i = paren + 1; i < offset && i < call_expr->end; i++) {
    if (module->source[i] == ',')
      comma_count++;
  }
  arg_index += comma_count;

  return SignatureInfo{
      .label = proc_signature(*proc),
      .documentation = "",
      .active_parameter = arg_index,
  };
}

std::vector<LspLocation>
IdeService::references(std::string_view abs_path, size_t line, size_t character,
                       bool include_declaration) const {
  auto sym = resolve_symbol(_project, abs_path, line, character);
  if (!sym.has_value())
    return {};
  return find_symbol_references(_project, *sym, include_declaration);
}

std::optional<LspRange>
IdeService::prepare_rename(std::string_view abs_path, size_t line,
                           size_t character) const {
  auto sym = resolve_symbol(_project, abs_path, line, character);
  if (!sym.has_value() || !sym->renamable)
    return std::nullopt;

  const ParsedModule *module = module_for_path(_project, abs_path);
  if (module == nullptr)
    return std::nullopt;

  size_t offset = position_to_offset(module->source, line, character);
  auto span = identifier_span_at(module->source, offset);
  if (!span.has_value())
    return std::nullopt;

  return make_range(module->source, span->first, span->second);
}

std::map<std::string, std::vector<std::pair<LspRange, std::string>>>
IdeService::rename(std::string_view abs_path, size_t line, size_t character,
                   std::string_view new_name) const {
  if (!is_valid_rename_name(new_name))
    return {};

  auto sym = resolve_symbol(_project, abs_path, line, character);
  if (!sym.has_value() || !sym->renamable)
    return {};

  std::map<std::string, std::vector<std::pair<LspRange, std::string>>> edits{};
  for (const auto &loc: find_symbol_references(_project, *sym, true)) {
    edits[loc.uri].push_back({loc.range, std::string(new_name)});
  }
  return edits;
}

SemanticTokens IdeService::semantic_tokens(std::string_view abs_path) const {
  const ParsedModule *module = module_for_path(_project, abs_path);
  if (module == nullptr)
    return {};

  std::vector<RawSemanticToken> raw{};
  for (Decl *decl: module->decls) {
    switch (decl->type) {
    case DECL_PROC: {
      auto *proc = std::get<decl::Proc *>(decl->data);
      add_sem_token(raw, proc->name.start, proc->name.end, SEM_FUNCTION,
                    MOD_DEFINITION);
      for (const Param &param: proc->params) {
        add_sem_token(raw, param.name.start, param.name.end, SEM_PARAMETER,
                      MOD_DECLARATION);
        collect_sem_tokens_in_type(param.type, raw);
      }
      if (proc->ret_type.has_value())
        collect_sem_tokens_in_type(proc->ret_type.value(), raw);
      collect_sem_tokens_in_stmts(proc->body, raw);
      break;
    }
    case DECL_STRUCT: {
      auto *strukt = std::get<decl::Struct *>(decl->data);
      add_sem_token(raw, strukt->name.start, strukt->name.end, SEM_STRUCT,
                    MOD_DEFINITION);
      for (const auto &field: strukt->fields) {
        add_sem_token(raw, field.name.start, field.name.end, SEM_PROPERTY,
                      MOD_DECLARATION);
        collect_sem_tokens_in_type(field.type, raw);
      }
      break;
    }
    case DECL_ENUM: {
      auto *enum_ = std::get<decl::Enum *>(decl->data);
      add_sem_token(raw, enum_->name.start, enum_->name.end, SEM_STRUCT,
                    MOD_DEFINITION);
      for (const auto &member: enum_->members) {
        add_sem_token(raw, member.name.start, member.name.end, SEM_PROPERTY,
                      MOD_DECLARATION);
      }
      break;
    }
    case DECL_CONST: {
      auto *konst = std::get<decl::Const *>(decl->data);
      add_sem_token(raw, konst->name.start, konst->name.end, SEM_VARIABLE,
                    MOD_READONLY | MOD_DECLARATION);
      collect_sem_tokens_in_type(konst->type, raw);
      collect_sem_tokens_in_expr(konst->value, raw);
      break;
    }
    case DECL_IMPORT:
      break;
    default:
      break;
    }
  }

  return encode_semantic_tokens(module->source, std::move(raw));
}

std::vector<CodeAction>
IdeService::code_actions(std::string_view abs_path,
                         const LspRange &range) const {
  const ParsedModule *module = module_for_path(_project, abs_path);
  if (module == nullptr)
    return {};

  std::string rel = project_rel_path(std::string(abs_path));
  std::vector<CodeAction> actions{};
  auto add_unique = [&](CodeAction action) {
    if (std::any_of(actions.begin(), actions.end(), [&](const CodeAction &existing) {
          return existing.title == action.title;
        }))
      return;
    actions.push_back(std::move(action));
  };

  for (const auto &diag: _project.diagnostics()) {
    if (!diagnostic_matches_file(diag, rel, abs_path))
      continue;

    LspRange diag_range = make_range(module->source, diag.start, diag.end);
    if (!ranges_overlap(diag_range, range))
      continue;

    for (const auto &help: diag.helps) {
      if (!help.replacement.has_value())
        continue;
      LspRange edit_range = make_range(module->source, help.start, help.end);
      add_unique(CodeAction{
          .title = help.message,
          .kind = "quickfix",
          .range = diag_range,
          .edits = {{edit_range, *help.replacement}},
          .is_preferred = true,
          .diagnostic_message = diag.message,
          .diagnostic_range = diag_range,
      });
    }
  }

  if (range.start_line == range.end_line) {
    auto word = word_at(*module, range.start_line, range.start_character);
    if (word.has_value()) {
      for (const auto &entry: _project.registry().modules()) {
        if (entry.first == module->module_name)
          continue;
        for (const auto &proc: entry.second.procs) {
          if (proc.name != word.value())
            continue;
          std::string import_path = entry.second.path;
          add_unique(CodeAction{
              .title = std::format("Import `{}` from `{}`", proc.name,
                                   entry.first),
              .kind = "quickfix",
              .range = range,
              .edits =
                  {{make_range(module->source, 0, 0),
                    std::format("import \"{}\";\n", import_path)}},
              .is_preferred = true,
          });
        }
        for (const auto &konst: entry.second.consts) {
          if (konst.name != word.value())
            continue;
          std::string import_path = entry.second.path;
          add_unique(CodeAction{
              .title = std::format("Import `{}` from `{}`", konst.name,
                                   entry.first),
              .kind = "quickfix",
              .range = range,
              .edits =
                  {{make_range(module->source, 0, 0),
                    std::format("import \"{}\";\n", import_path)}},
          });
        }
      }
    }
  }

  return actions;
}

std::vector<InlayHint> IdeService::inlay_hints(
    std::string_view abs_path, const IdeInlayHintOptions &options,
    const std::optional<LspRange> &range) const {
  const ParsedModule *module = module_for_path(_project, abs_path);
  if (module == nullptr)
    return {};

  std::vector<InlayHint> hints{};
  for (Decl *decl: module->decls) {
    if (decl->type == DECL_PROC) {
      auto *proc = std::get<decl::Proc *>(decl->data);
      collect_inlay_hints_in_stmts(_project, *module, proc->body, options,
                                  hints);
    } else if (decl->type == DECL_CONST) {
      auto *konst = std::get<decl::Const *>(decl->data);
      collect_inlay_hints_in_expr(_project, *module, konst->value, options,
                                  hints);
    }
  }

  if (!range.has_value())
    return hints;

  std::vector<InlayHint> filtered{};
  for (const InlayHint &hint: hints) {
    if (inlay_hint_in_range(hint, *range))
      filtered.push_back(hint);
  }
  return filtered;
}

bool IdeService::is_valid_rename_name(std::string_view name) {
  if (name.empty())
    return false;
  unsigned char first = static_cast<unsigned char>(name[0]);
  if (!std::isalpha(first) && name[0] != '_')
    return false;
  for (size_t i = 1; i < name.size(); i++) {
    unsigned char ch = static_cast<unsigned char>(name[i]);
    if (!std::isalnum(ch) && name[i] != '_')
      return false;
  }
  static constexpr std::string_view keywords[] = {
      "proc", "var", "const", "if", "else", "for", "while", "return",
      "import", "struct", "enum", "union", "extern", "comptime", "when",
      "break", "continue", "cast", "sizeof", "true", "false"};
  for (std::string_view keyword: keywords) {
    if (name == keyword)
      return false;
  }
  return true;
}

std::vector<WorkspaceSymbol>
IdeService::workspace_symbols(std::string_view query) const {
  std::vector<WorkspaceSymbol> out{};
  for (const auto &[path, module]: _project.modules()) {
    auto add = [&](const std::string &name, int kind, size_t start, size_t end,
                   const std::string &container) {
      if (!symbol_name_matches(name, query))
        return;
      out.push_back(WorkspaceSymbol{
          .name = name,
          .container_name = container,
          .kind = kind,
          .location =
              LspLocation{.uri = lsp_path_to_uri(module.abs_path),
                          .range = make_range(module.source, start, end)},
      });
    };
    for (Decl *decl: module.decls) {
      switch (decl->type) {
      case DECL_PROC: {
        auto *proc = std::get<decl::Proc *>(decl->data);
        add(std::string(proc->name.id_value), 12, proc->name.start,
            proc->name.end, module.module_name);
        break;
      }
      case DECL_STRUCT: {
        auto *strukt = std::get<decl::Struct *>(decl->data);
        add(std::string(strukt->name.id_value), 22, strukt->name.start,
            strukt->name.end, module.module_name);
        for (const auto &field: strukt->fields)
          add(std::string(field.name.id_value), 8, field.name.start,
              field.name.end, std::string(strukt->name.id_value));
        break;
      }
      case DECL_ENUM: {
        auto *enum_ = std::get<decl::Enum *>(decl->data);
        add(std::string(enum_->name.id_value), 10, enum_->name.start,
            enum_->name.end, module.module_name);
        break;
      }
      case DECL_CONST: {
        auto *konst = std::get<decl::Const *>(decl->data);
        add(std::string(konst->name.id_value), 14, konst->name.start,
            konst->name.end, module.module_name);
        break;
      }
      default:
        break;
      }
    }
  }
  return out;
}

std::optional<LspLocation> IdeService::type_definition(std::string_view abs_path,
                                                      size_t line,
                                                      size_t character) const {
  const ParsedModule *module = module_for_path(_project, abs_path);
  if (module == nullptr)
    return std::nullopt;

  size_t offset = position_to_offset(module->source, line, character);
  if (auto type = type_at_offset(*module, offset))
    return location_for_type(_project, *type);

  return std::nullopt;
}

std::vector<DocumentHighlight>
IdeService::document_highlight(std::string_view abs_path, size_t line,
                               size_t character) const {
  std::vector<DocumentHighlight> out{};
  for (const auto &loc: references(abs_path, line, character, true)) {
    if (lsp_uri_to_path(loc.uri) != abs_path)
      continue;
    out.push_back(DocumentHighlight{.range = loc.range, .kind = 1});
  }
  return out;
}

std::vector<FoldingRange>
IdeService::folding_ranges(std::string_view abs_path) const {
  const ParsedModule *module = module_for_path(_project, abs_path);
  if (module == nullptr)
    return {};

  std::vector<FoldingRange> out{};
  for (Decl *decl: module->decls) {
    if (decl->type == DECL_PROC) {
      auto *proc = std::get<decl::Proc *>(decl->data);
      auto [start_line, _] =
          offset_to_line_column(module->source, decl->start);
      auto [end_line, __] = offset_to_line_column(module->source, decl->end);
      if (end_line > start_line)
        out.push_back(FoldingRange{.start_line = start_line,
                                   .end_line = end_line,
                                   .kind = 0});
      collect_folding_in_stmts(*module, proc->body, out);
    }
  }
  return out;
}

std::optional<std::string>
IdeService::format_document(std::string_view abs_path) const {
  const ParsedModule *module = module_for_path(_project, abs_path);
  if (module == nullptr)
    return std::nullopt;
  return format_rye_source(module->source);
}

SemanticTokens IdeService::semantic_tokens_range(std::string_view abs_path,
                                                 const LspRange &range) const {
  return semantic_tokens(abs_path);
}

SemanticTokensDelta IdeService::semantic_tokens_delta(
    std::string_view abs_path, std::string_view previous_id,
    const std::vector<uint32_t> &previous_data) const {
  SemanticTokens current = semantic_tokens(abs_path);
  std::string new_id =
      previous_id.empty() ? "1" : std::string(previous_id);
  if (!previous_id.empty()) {
    try {
      new_id = std::to_string(std::stoll(std::string(previous_id)) + 1);
    } catch (...) {
      new_id = "1";
    }
  }

  if (previous_data == current.data && !previous_id.empty()) {
    return SemanticTokensDelta{.result_id = std::string(previous_id),
                                 .full_rebuild = false};
  }

  return SemanticTokensDelta{
      .result_id = new_id,
      .data = current.data,
      .start = 0,
      .delete_count = previous_data.size(),
      .full_rebuild = false,
  };
}

std::optional<CallHierarchyItem>
IdeService::prepare_call_hierarchy(std::string_view abs_path, size_t line,
                                   size_t character) const {
  auto sym = resolve_symbol(_project, abs_path, line, character);
  if (!sym.has_value() || sym->kind != SymKind::Proc)
    return std::nullopt;

  const ParsedModule *module = module_for_path(_project, sym->file_path);
  if (module == nullptr)
    return std::nullopt;

  auto proc = _project.registry().find_proc(sym->module_name, sym->name);
  std::string detail =
      proc.has_value() ? proc_signature(*proc) : sym->name;

  return CallHierarchyItem{
      .name = sym->name,
      .detail = detail,
      .uri = lsp_path_to_uri(sym->file_path),
      .range = make_range(module->source, sym->def_start, sym->def_end),
      .selection_range =
          make_range(module->source, sym->def_start, sym->def_end),
      .data = std::format("{}:{}", sym->module_name, sym->name),
  };
}

std::vector<CallHierarchyCall>
IdeService::call_hierarchy_incoming(std::string_view /*abs_path*/,
                                    const CallHierarchyItem &item) const {
  std::vector<CallHierarchyCall> out{};
  auto colon = item.data.find(':');
  if (colon == std::string::npos)
    return out;
  std::string_view module_name(item.data.data(), colon);
  std::string_view proc_name(item.data.data() + colon + 1,
                             item.data.size() - colon - 1);
  collect_incoming_calls(_project, module_name, proc_name, out);
  return out;
}

std::vector<CallHierarchyCall>
IdeService::call_hierarchy_outgoing(std::string_view /*abs_path*/,
                                    const CallHierarchyItem &item) const {
  const ParsedModule *module =
      module_for_path(_project, lsp_uri_to_path(item.uri));
  if (module == nullptr)
    return {};

  auto colon = item.data.find(':');
  if (colon == std::string::npos)
    return {};

  std::string_view proc_name(item.data.data() + colon + 1,
                             item.data.size() - colon - 1);

  std::vector<CallHierarchyCall> out{};
  std::set<std::string> seen{};
  for (Decl *decl: module->decls) {
    if (decl->type != DECL_PROC)
      continue;
    auto *proc = std::get<decl::Proc *>(decl->data);
    if (proc->name.id_value != proc_name)
      continue;
    collect_outgoing_calls_in_stmts(_project, *module, proc->body, out, seen);
    break;
  }
  return out;
}

std::string apply_lsp_range_edit(std::string_view source, const LspRange &range,
                                 std::string_view new_text) {
  size_t start =
      position_to_offset(source, range.start_line, range.start_character);
  size_t end =
      position_to_offset(source, range.end_line, range.end_character);
  return std::string(source.substr(0, start)) + std::string(new_text) +
         std::string(source.substr(end));
}
