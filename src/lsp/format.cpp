#include <lsp/format.hpp>

#include <algorithm>
#include <cctype>
#include <format>
#include <vector>

#include <arena.hpp>
#include <lexer/lexer.hpp>
#include <parser/ast.hpp>
#include <parser/parser.hpp>

namespace {

struct Comment {
  size_t start;
  size_t end;
};

std::vector<Comment> collect_comments(std::string_view source) {
  std::vector<Comment> comments{};
  for (size_t i = 0; i + 1 < source.size(); i++) {
    if (source[i] == '/' && source[i + 1] == '/') {
      size_t start = i;
      i += 2;
      while (i < source.size() && source[i] != '\n')
        i++;
      comments.push_back({start, i});
    }
  }
  return comments;
}

constexpr size_t SOFT_LINE_LIMIT = 88;
constexpr size_t STRUCT_LIT_LINE_LIMIT = 72;

std::string expr_shape_key(Expr *expr);

bool source_has_blank_gap(std::string_view source, size_t from, size_t to) {
  if (from >= to || to > source.size())
    return false;
  std::string_view gap = source.substr(from, to - from);
  for (size_t i = 0; i + 1 < gap.size(); i++) {
    if (gap[i] == '\n' && gap[i + 1] == '\n')
      return true;
    if (gap[i] == '\r' && gap[i + 1] == '\n' && i + 2 < gap.size() &&
        gap[i + 2] == '\r' && i + 3 < gap.size() && gap[i + 3] == '\n')
      return true;
  }
  return false;
}

size_t estimate_expr_width(Expr *expr, std::string_view source) {
  if (expr == nullptr)
    return 0;
  switch (expr->type) {
  case EXPR_INT:
    return std::to_string(std::get<expr::Int *>(expr->data)->value).size();
  case EXPR_BOOL:
    return 4;
  case EXPR_VAR: {
    auto *var = std::get<expr::Var *>(expr->data);
    size_t w = var->var.id_value.size();
    if (var->module.has_value())
      w += var->module->id_value.size() + 1;
    return w;
  }
  case EXPR_ENUM_CASE:
    return 1 + std::get<expr::EnumCase *>(expr->data)->member.id_value.size();
  case EXPR_STRING:
    return std::get<expr::String *>(expr->data)->value.end -
           std::get<expr::String *>(expr->data)->value.start;
  case EXPR_GROUP:
    return 2 + estimate_expr_width(std::get<expr::Group *>(expr->data)->expr, source);
  case EXPR_BINARY: {
    auto *bin = std::get<expr::Binary *>(expr->data);
    return estimate_expr_width(bin->lhs, source) +
           1 + binop_to_string(bin->op).size() +
           1 + estimate_expr_width(bin->rhs, source);
  }
  case EXPR_REF:
    return 1 + estimate_expr_width(std::get<expr::Ref *>(expr->data)->expr, source);
  case EXPR_DEREF:
    return 1 + estimate_expr_width(std::get<expr::Deref *>(expr->data)->expr, source);
  case EXPR_NOT:
    return 1 + estimate_expr_width(std::get<expr::Not *>(expr->data)->expr, source);
  case EXPR_FIELD: {
    auto *field = std::get<expr::Field *>(expr->data);
    return estimate_expr_width(field->base, source) + 1 +
           field->field.id_value.size();
  }
  case EXPR_CALL: {
    auto *call = std::get<expr::Call *>(expr->data);
    size_t w = 0;
    if (call->callee != nullptr)
      w = estimate_expr_width(call->callee, source);
    else if (call->receiver != nullptr)
      w = estimate_expr_width(call->receiver, source) + 1 +
          call->name->id_value.size();
    else if (call->name.has_value()) {
      if (call->module.has_value())
        w += call->module->id_value.size() + 1;
      w += call->name->id_value.size();
    }
    w += 2;
    for (size_t i = 0; i < call->arguments.size(); i++) {
      if (i > 0)
        w += 2;
      w += estimate_expr_width(call->arguments[i], source);
    }
    return w;
  }
  case EXPR_COMPTIME_CALL: {
    auto *call = std::get<expr::ComptimeCall *>(expr->data);
    size_t w = 9 + call->name.id_value.size() + 2;
    for (size_t i = 0; i < call->arguments.size(); i++) {
      if (i > 0)
        w += 2;
      w += estimate_expr_width(call->arguments[i], source);
    }
    return w;
  }
  case EXPR_STRUCT_LIT: {
    auto *lit = std::get<expr::StructLit *>(expr->data);
    size_t w = lit->type_name.id_value.size() + 2;
    for (size_t i = 0; i < lit->fields.size(); i++) {
      if (i > 0)
        w += 2;
      w += lit->fields[i].name.id_value.size() + 3 +
           estimate_expr_width(lit->fields[i].value, source);
    }
    w += 1;
    return w;
  }
  case EXPR_CAST: {
    auto *cast_ = std::get<expr::Cast *>(expr->data);
    return 6 + cast_->target->to_string().size() + 2 +
           estimate_expr_width(cast_->expr, source);
  }
  case EXPR_SIZEOF:
    return 8 + std::get<expr::Sizeof *>(expr->data)->type->to_string().size() + 1;
  case EXPR_INDEX: {
    auto *index = std::get<expr::Index *>(expr->data);
    return estimate_expr_width(index->base, source) + 2 +
           estimate_expr_width(index->index, source);
  }
  case EXPR_TUPLE: {
    auto *tuple = std::get<expr::TupleLit *>(expr->data);
    size_t w = 2;
    for (size_t i = 0; i < tuple->elements.size(); i++) {
      if (i > 0)
        w += 2;
      w += estimate_expr_width(tuple->elements[i], source);
    }
    return w;
  }
  case EXPR_PROC_LIT:
    return 12;
  }
  return expr_shape_key(expr).size();
}

bool is_compact_stmt(Stmt *stmt, std::string_view source) {
  if (stmt == nullptr)
    return false;
  switch (stmt->type) {
  case STMT_RETURN: {
    auto *ret = std::get<stmt::Return *>(stmt->data);
    return !ret->value.has_value() ||
           estimate_expr_width(ret->value.value(), source) < 40;
  }
  case STMT_BREAK:
  case STMT_CONTINUE:
    return true;
  case STMT_ASSIGN: {
    auto *assign = std::get<stmt::Assign *>(stmt->data);
    return estimate_expr_width(assign->target, source) +
               estimate_expr_width(assign->value, source) <
           48;
  }
  case STMT_EXPR:
    return estimate_expr_width(
               std::get<stmt::ExprStmt *>(stmt->data)->expr, source) < 56;
  default:
    return false;
  }
}

bool is_compact_if_branch(const std::vector<Stmt *> &block, std::string_view source) {
  return block.size() == 1 && is_compact_stmt(block.front(), source);
}

bool is_compact_if_chain(stmt::If *if_, std::string_view source) {
  if (!is_compact_if_branch(if_->then_block, source))
    return false;
  if (if_->else_block.empty())
    return true;
  if (if_->else_block.size() == 1 && if_->else_block.front()->type == STMT_IF)
    return is_compact_if_chain(std::get<stmt::If *>(if_->else_block.front()->data),
                               source);
  return is_compact_if_branch(if_->else_block, source);
}

bool types_equal(Type *a, Type *b) {
  if (a == b)
    return true;
  if (a == nullptr || b == nullptr)
    return false;
  return a->to_string() == b->to_string();
}

enum class StmtGroupKind {
  Vars,
  FieldAssigns,
  Assigns,
  Control,
  Comptime,
  Returns,
  BreakContinue,
  Exprs,
  Singleton,
};

struct StmtGroup {
  StmtGroupKind kind;
  std::vector<Stmt *> stmts;
};

enum class DeclGroupKind {
  Imports,
  TypeDefs,
  Consts,
  Comptime,
  Procs,
  When,
  Other,
};

struct DeclGroup {
  DeclGroupKind kind;
  std::vector<Decl *> decls;
};

struct VarDeclView {
  Stmt *stmt;
  stmt::Var *var;
};

bool is_var_stmt(Stmt *stmt) { return stmt != nullptr && stmt->type == STMT_VAR; }

bool is_var_block(Stmt *stmt) {
  if (stmt == nullptr || stmt->type != STMT_BLOCK)
    return false;
  auto *block = std::get<stmt::Block *>(stmt->data);
  if (block->scoped || block->stmts.empty())
    return false;
  return std::all_of(block->stmts.begin(), block->stmts.end(), is_var_stmt);
}

bool is_control_stmt(Stmt *stmt) {
  if (stmt == nullptr)
    return false;
  switch (stmt->type) {
  case STMT_IF:
  case STMT_WHILE:
  case STMT_FOR:
  case STMT_FOR_IN:
  case STMT_WHEN:
  case STMT_COMPTIME_BLOCK:
    return true;
  default:
    return false;
  }
}

Expr *assign_target(Stmt *stmt) {
  if (stmt == nullptr || stmt->type != STMT_ASSIGN)
    return nullptr;
  return std::get<stmt::Assign *>(stmt->data)->target;
}

bool is_field_assign(Stmt *stmt) {
  Expr *target = assign_target(stmt);
  return target != nullptr && target->type == EXPR_FIELD;
}

std::string expr_shape_key(Expr *expr) {
  if (expr == nullptr)
    return "";
  return expr->to_string();
}

bool same_assign_base(Expr *a, Expr *b) {
  if (a == nullptr || b == nullptr)
    return false;
  if (a->type == EXPR_FIELD && b->type == EXPR_FIELD) {
    auto *fa = std::get<expr::Field *>(a->data);
    auto *fb = std::get<expr::Field *>(b->data);
    return expr_shape_key(fa->base) == expr_shape_key(fb->base);
  }
  return expr_shape_key(a) == expr_shape_key(b);
}

bool is_simple_init(Expr *expr, std::string_view source) {
  if (expr == nullptr)
    return false;
  switch (expr->type) {
  case EXPR_INT:
  case EXPR_BOOL:
  case EXPR_VAR:
  case EXPR_ENUM_CASE:
  case EXPR_REF:
  case EXPR_NOT:
  case EXPR_DEREF:
    return true;
  case EXPR_CALL: {
    auto *call = std::get<expr::Call *>(expr->data);
    return call->arguments.size() <= 3 &&
           std::all_of(call->arguments.begin(), call->arguments.end(),
                       [&](Expr *arg) { return is_simple_init(arg, source); });
  }
  case EXPR_FIELD:
    return is_simple_init(std::get<expr::Field *>(expr->data)->base, source);
  case EXPR_STRUCT_LIT: {
    auto *lit = std::get<expr::StructLit *>(expr->data);
    if (source.substr(expr->start, expr->end - expr->start).contains('\n'))
      return false;
  return lit->fields.size() <= 2 &&
         std::all_of(lit->fields.begin(), lit->fields.end(),
                     [&](const expr::StructLitField &f) {
                       return is_simple_init(f.value, source);
                     });
  }
  case EXPR_BINARY: {
    auto *bin = std::get<expr::Binary *>(expr->data);
    return is_simple_init(bin->lhs, source) && is_simple_init(bin->rhs, source);
  }
  case EXPR_CAST:
    return is_simple_init(std::get<expr::Cast *>(expr->data)->expr, source);
  default:
    return false;
  }
}

bool struct_lit_should_multiline(Expr *expr, expr::StructLit *lit,
                                 std::string_view source) {
  if (source.substr(expr->start, expr->end - expr->start).contains('\n'))
    return true;
  if (lit->fields.size() > 2)
    return true;
  if (estimate_expr_width(expr, source) > STRUCT_LIT_LINE_LIMIT)
    return true;
  return std::any_of(lit->fields.begin(), lit->fields.end(),
                     [&](const expr::StructLitField &f) {
                       return !is_simple_init(f.value, source);
                     });
}

int import_sort_rank(const decl::Import *import) {
  std::string_view path = import->path.string_value;
  if (path.starts_with("std/"))
    return 0;
  if (path.starts_with("runtime/"))
    return 1;
  return 2;
}

void sort_import_decls(std::vector<Decl *> &imports) {
  std::stable_sort(imports.begin(), imports.end(),
                   [](Decl *a, Decl *b) {
                     auto *ia = std::get<decl::Import *>(a->data);
                     auto *ib = std::get<decl::Import *>(b->data);
                     int ra = import_sort_rank(ia);
                     int rb = import_sort_rank(ib);
                     if (ra != rb)
                       return ra < rb;
                     return ia->path.string_value < ib->path.string_value;
                   });
}

std::string assign_lhs_text(Expr *target) {
  if (target == nullptr)
    return "";
  if (target->type == EXPR_FIELD) {
    auto *field = std::get<expr::Field *>(target->data);
    return std::format("{}.{}", assign_lhs_text(field->base), field->field.id_value);
  }
  return expr_shape_key(target);
}

size_t estimate_var_decl_width(const stmt::Var *var, std::string_view source) {
  size_t width = 4 + var->name.id_value.size();
  if (var->type != nullptr)
    width += 1 + var->type->to_string().size();
  if (var->value.has_value())
    width += 3 + expr_shape_key(var->value.value()).size();
  return width;
}

bool can_share_var_line(const stmt::Var *a, const stmt::Var *b,
                        std::string_view source) {
  if (!types_equal(a->type, b->type))
    return false;
  if (a->value.has_value() != b->value.has_value())
    return false;
  if (a->value.has_value()) {
    if (!is_simple_init(a->value.value(), source) ||
        !is_simple_init(b->value.value(), source))
      return false;
  }
  return estimate_var_decl_width(a, source) +
             estimate_var_decl_width(b, source) <
         96;
}

std::vector<VarDeclView> flatten_var_stmts(const std::vector<Stmt *> &stmts) {
  std::vector<VarDeclView> views{};
  for (Stmt *stmt: stmts) {
    if (is_var_stmt(stmt))
      views.push_back({stmt, std::get<stmt::Var *>(stmt->data)});
    else if (is_var_block(stmt)) {
      auto *block = std::get<stmt::Block *>(stmt->data);
      for (Stmt *inner: block->stmts)
        views.push_back({inner, std::get<stmt::Var *>(inner->data)});
    }
  }
  return views;
}

std::vector<StmtGroup> partition_stmt_groups(const std::vector<Stmt *> &stmts) {
  std::vector<StmtGroup> groups{};
  size_t i = 0;
  while (i < stmts.size()) {
    Stmt *stmt = stmts[i];

    if (is_var_stmt(stmt) || is_var_block(stmt)) {
      size_t j = i + 1;
      while (j < stmts.size() &&
             (is_var_stmt(stmts[j]) || is_var_block(stmts[j])))
        j++;
      groups.push_back({StmtGroupKind::Vars, {stmts.begin() + i, stmts.begin() + j}});
      i = j;
      continue;
    }

    if (is_field_assign(stmt)) {
      Expr *base = assign_target(stmt);
      size_t j = i + 1;
      while (j < stmts.size() && is_field_assign(stmts[j]) &&
             same_assign_base(assign_target(stmts[j]), base))
        j++;
      groups.push_back(
          {StmtGroupKind::FieldAssigns, {stmts.begin() + i, stmts.begin() + j}});
      i = j;
      continue;
    }

    if (is_control_stmt(stmt)) {
      groups.push_back({StmtGroupKind::Control, {stmt}});
      i++;
      continue;
    }

    if (stmt->type == STMT_RETURN) {
      groups.push_back({StmtGroupKind::Returns, {stmt}});
      i++;
      continue;
    }

    if (stmt->type == STMT_BREAK || stmt->type == STMT_CONTINUE) {
      groups.push_back({StmtGroupKind::BreakContinue, {stmt}});
      i++;
      continue;
    }

    if (stmt->type == STMT_EXPR) {
      size_t j = i + 1;
      while (j < stmts.size() && stmts[j]->type == STMT_EXPR)
        j++;
      groups.push_back({StmtGroupKind::Exprs, {stmts.begin() + i, stmts.begin() + j}});
      i = j;
      continue;
    }

    if (stmt->type == STMT_ASSIGN) {
      size_t j = i + 1;
      while (j < stmts.size() && stmts[j]->type == STMT_ASSIGN &&
             !is_field_assign(stmts[j]))
        j++;
      groups.push_back({StmtGroupKind::Assigns, {stmts.begin() + i, stmts.begin() + j}});
      i = j;
      continue;
    }

    groups.push_back({StmtGroupKind::Singleton, {stmt}});
    i++;
  }
  return groups;
}

bool should_blank_between_stmt_groups(const StmtGroup &prev, const StmtGroup &next) {
  StmtGroupKind prev_kind = prev.kind;
  StmtGroupKind next_kind = next.kind;

  if (prev_kind == next_kind) {
    if (prev_kind == StmtGroupKind::Control)
      return false;
    if (prev_kind == StmtGroupKind::Exprs || prev_kind == StmtGroupKind::FieldAssigns ||
        prev_kind == StmtGroupKind::Assigns)
      return false;
    return false;
  }

  if (prev_kind == StmtGroupKind::Vars && next_kind == StmtGroupKind::Control) {
    if (!next.stmts.empty() && next.stmts.front()->type == STMT_IF)
      return false;
    return true;
  }

  if (prev_kind == StmtGroupKind::Vars &&
      (next_kind == StmtGroupKind::Assigns || next_kind == StmtGroupKind::FieldAssigns ||
       next_kind == StmtGroupKind::Exprs || next_kind == StmtGroupKind::Returns))
    return true;

  if (prev_kind == StmtGroupKind::Control && next_kind == StmtGroupKind::Vars)
    return true;

  if ((prev_kind == StmtGroupKind::Assigns || prev_kind == StmtGroupKind::FieldAssigns ||
       prev_kind == StmtGroupKind::Exprs) &&
      next_kind == StmtGroupKind::Returns)
    return true;

  if (prev_kind == StmtGroupKind::Control && next_kind == StmtGroupKind::Returns)
    return false;

  if (prev_kind == StmtGroupKind::Comptime)
    return true;

  if (next_kind == StmtGroupKind::Returns &&
      prev_kind != StmtGroupKind::BreakContinue &&
      prev_kind != StmtGroupKind::Returns)
    return true;

  return false;
}

DeclGroupKind decl_group_kind(Decl *decl) {
  switch (decl->type) {
  case DECL_IMPORT:         return DeclGroupKind::Imports;
  case DECL_STRUCT:
  case DECL_ENUM:           return DeclGroupKind::TypeDefs;
  case DECL_CONST:          return DeclGroupKind::Consts;
  case DECL_COMPTIME_BLOCK: return DeclGroupKind::Comptime;
  case DECL_PROC:           return DeclGroupKind::Procs;
  case DECL_WHEN:           return DeclGroupKind::When;
  default:                  return DeclGroupKind::Other;
  }
}

std::vector<DeclGroup> partition_decl_groups(const std::vector<Decl *> &decls) {
  std::vector<DeclGroup> groups{};
  for (Decl *decl: decls) {
    DeclGroupKind kind = decl_group_kind(decl);
    if (!groups.empty() && groups.back().kind == kind &&
        (kind == DeclGroupKind::Imports || kind == DeclGroupKind::Consts))
      groups.back().decls.push_back(decl);
    else
      groups.push_back({kind, {decl}});
  }
  return groups;
}

bool should_blank_between_decl_groups(DeclGroupKind prev, DeclGroupKind next) {
  if (prev == next) {
    if (prev == DeclGroupKind::TypeDefs || prev == DeclGroupKind::Procs)
      return true;
    return false;
  }
  return true;
}

class RyeFormatter {
public:
  RyeFormatter(std::string_view source, const std::vector<Comment> &comments) :
      _source(source), _comments(comments) {}

  std::string format_decls(const std::vector<Decl *> &decls) {
    std::vector<DeclGroup> groups = partition_decl_groups(decls);
    for (size_t g = 0; g < groups.size(); g++) {
      if (groups[g].kind == DeclGroupKind::Imports)
        sort_import_decls(groups[g].decls);
      for (Decl *decl: groups[g].decls)
        format_decl(decl);
      if (g + 1 < groups.size() &&
          should_blank_between_decl_groups(groups[g].kind, groups[g + 1].kind))
        newline();
    }
    emit_comments_until(_source.size());
    trim_trailing_blank_lines();
    if (_out.empty() || _out.back() != '\n')
      _out += '\n';
    return _out;
  }

private:
  std::string_view _source;
  const std::vector<Comment> &_comments;
  size_t _comment_idx = 0;
  std::string _out;
  int _indent = 0;
  bool _at_line_start = true;
  size_t _current_line_width = 0;

  void reset_line_width() { _current_line_width = _indent * 2; }

  void trim_trailing_blank_lines() {
    while (_out.size() >= 2 && _out.ends_with("\n\n"))
      _out.pop_back();
  }

  void emit_comments_until(size_t pos) {
    while (_comment_idx < _comments.size() &&
           _comments[_comment_idx].start < pos) {
      const Comment &comment = _comments[_comment_idx];
      if (!_at_line_start)
        newline();
      write(_source.substr(comment.start, comment.end - comment.start));
      newline();
      _comment_idx++;
    }
  }

  void write(std::string_view text) {
    _at_line_start = false;
    _current_line_width += text.size();
    _out.append(text.data(), text.size());
  }

  void write(char ch) {
    _at_line_start = ch == '\n';
    if (ch == '\n')
      _current_line_width = 0;
    else
      _current_line_width++;
    _out += ch;
  }

  void newline() {
    _out += '\n';
    _at_line_start = true;
    _current_line_width = 0;
  }

  void write_indent() {
    for (int i = 0; i < _indent; i++)
      write("  ");
    _at_line_start = false;
    _current_line_width = _indent * 2;
  }

  void begin_line() {
    if (!_at_line_start)
      newline();
    write_indent();
  }

  void write_padding(size_t columns) {
    while (_current_line_width < columns)
      write(' ');
  }


  void write_source_span(size_t start, size_t end) {
    write(_source.substr(start, end - start));
  }

  void write_name_type_groups(const std::vector<std::pair<std::string_view, Type *>> &items,
                              bool trailing_comma_between_groups = false) {
    size_t i = 0;
    while (i < items.size()) {
      size_t j = i + 1;
      while (j < items.size() && types_equal(items[j].second, items[i].second))
        j++;
      for (size_t k = i; k < j; k++) {
        write(items[k].first);
        if (k + 1 < j)
          write(", ");
      }
      write(' ');
      format_type(items[i].second);
      if (j < items.size()) {
        if (trailing_comma_between_groups)
          write(", ");
        else
          write(", ");
      }
      i = j;
    }
  }

  void format_type(Type *type) {
    if (type == nullptr)
      return;
    emit_comments_until(type->start);
    switch (type->type) {
    case TYPE_VOID: write("void"); break;
    case TYPE_BOOL: write("bool"); break;
    case TYPE_INT:  write("int"); break;
    case TYPE_BYTE: write("byte"); break;
    case TYPE_PTR: {
      write('*');
      format_type(std::get<type::Ptr *>(type->data)->inner);
      break;
    }
    case TYPE_STRUCT:
      write(std::get<type::Struct *>(type->data)->name);
      break;
    case TYPE_ENUM:
      write(std::get<type::Enum *>(type->data)->name);
      break;
    case TYPE_UNION: {
      auto *union_type = std::get<type::Union *>(type->data);
      write("union { ");
      for (size_t i = 0; i < union_type->members.size(); i++) {
        if (i > 0)
          write(", ");
        format_type(union_type->members[i]);
      }
      write(" }");
      break;
    }
    case TYPE_PROC: {
      auto *proc = std::get<type::Proc *>(type->data);
      write("proc(");
      std::vector<std::pair<std::string_view, Type *>> params{};
      for (const Param &param: proc->params)
        params.emplace_back(param.name.id_value, param.type);
      write_name_type_groups(params);
      write(") ");
      format_type(proc->ret_type);
      break;
    }
    case TYPE_TUPLE: {
      auto *tuple = std::get<type::Tuple *>(type->data);
      write('(');
      for (size_t i = 0; i < tuple->elements.size(); i++) {
        if (i > 0)
          write(", ");
        format_type(tuple->elements[i]);
      }
      write(')');
      break;
    }
    }
  }

  void format_expr(Expr *expr) {
    if (expr == nullptr)
      return;
    emit_comments_until(expr->start);
    switch (expr->type) {
    case EXPR_INT:
      write(std::to_string(std::get<expr::Int *>(expr->data)->value));
      break;
    case EXPR_BOOL:
      write(std::get<expr::Bool *>(expr->data)->value ? "true" : "false");
      break;
    case EXPR_VAR: {
      auto *var = std::get<expr::Var *>(expr->data);
      if (var->module.has_value()) {
        write(var->module->id_value);
        write(':');
      }
      write(var->var.id_value);
      break;
    }
    case EXPR_GROUP: {
      write('(');
      format_expr(std::get<expr::Group *>(expr->data)->expr);
      write(')');
      break;
    }
    case EXPR_BINARY: {
      auto *bin = std::get<expr::Binary *>(expr->data);
      size_t total = estimate_expr_width(expr, _source);
      bool break_line =
          total + _current_line_width > SOFT_LINE_LIMIT &&
          (bin->op == expr::Binary::BINOP_OR || bin->op == expr::Binary::BINOP_AND);
      if (break_line) {
        format_expr(bin->lhs);
        begin_line();
        write(binop_to_string(bin->op));
        write(' ');
        format_expr(bin->rhs);
      } else {
        format_expr(bin->lhs);
        write(' ');
        write(binop_to_string(bin->op));
        write(' ');
        format_expr(bin->rhs);
      }
      break;
    }
    case EXPR_REF: {
      write('&');
      format_expr(std::get<expr::Ref *>(expr->data)->expr);
      break;
    }
    case EXPR_DEREF: {
      write('*');
      format_expr(std::get<expr::Deref *>(expr->data)->expr);
      break;
    }
    case EXPR_CALL: {
      auto *call = std::get<expr::Call *>(expr->data);
      if (call->callee != nullptr)
        format_expr(call->callee);
      else if (call->receiver != nullptr) {
        format_expr(call->receiver);
        write('.');
        write(call->name->id_value);
      } else if (call->name.has_value()) {
        if (call->module.has_value()) {
          write(call->module->id_value);
          write(':');
        }
        write(call->name->id_value);
      }
      write('(');
      for (size_t i = 0; i < call->arguments.size(); i++) {
        if (i > 0)
          write(", ");
        format_expr(call->arguments[i]);
      }
      write(')');
      break;
    }
    case EXPR_COMPTIME_CALL: {
      auto *call = std::get<expr::ComptimeCall *>(expr->data);
      write("comptime ");
      write(call->name.id_value);
      write('(');
      for (size_t i = 0; i < call->arguments.size(); i++) {
        if (i > 0)
          write(", ");
        format_expr(call->arguments[i]);
      }
      write(')');
      break;
    }
    case EXPR_NOT: {
      write('!');
      format_expr(std::get<expr::Not *>(expr->data)->expr);
      break;
    }
    case EXPR_FIELD: {
      auto *field = std::get<expr::Field *>(expr->data);
      format_expr(field->base);
      write('.');
      write(field->field.id_value);
      break;
    }
    case EXPR_STRUCT_LIT:
      format_struct_lit_expr(expr);
      break;
    case EXPR_STRING: {
      auto *str = std::get<expr::String *>(expr->data);
      write_source_span(str->value.start, str->value.end);
      break;
    }
    case EXPR_SIZEOF: {
      auto *sizeof_ = std::get<expr::Sizeof *>(expr->data);
      write("sizeof(");
      format_type(sizeof_->type);
      write(')');
      break;
    }
    case EXPR_CAST: {
      auto *cast_ = std::get<expr::Cast *>(expr->data);
      write("cast(");
      format_type(cast_->target);
      write(')');
      write(' ');
      format_expr(cast_->expr);
      break;
    }
    case EXPR_ENUM_CASE:
      write('.');
      write(std::get<expr::EnumCase *>(expr->data)->member.id_value);
      break;
    case EXPR_INDEX: {
      auto *index = std::get<expr::Index *>(expr->data);
      format_expr(index->base);
      write('[');
      format_expr(index->index);
      write(']');
      break;
    }
    case EXPR_PROC_LIT: {
      auto *lit = std::get<expr::ProcLit *>(expr->data);
      write("proc(");
      std::vector<std::pair<std::string_view, Type *>> params{};
      for (const Param &param: lit->params)
        params.emplace_back(param.name.id_value, param.type);
      write_name_type_groups(params);
      write(") ");
      format_type(lit->ret_type);
      write(' ');
      format_block_stmts(lit->body);
      break;
    }
    case EXPR_TUPLE: {
      auto *tuple = std::get<expr::TupleLit *>(expr->data);
      write('(');
      for (size_t i = 0; i < tuple->elements.size(); i++) {
        if (i > 0)
          write(", ");
        format_expr(tuple->elements[i]);
      }
      write(')');
      break;
    }
    }
  }

  void format_struct_lit_expr(Expr *expr) {
    auto *lit = std::get<expr::StructLit *>(expr->data);
    bool multiline = struct_lit_should_multiline(expr, lit, _source);
    write(lit->type_name.id_value);
    if (!multiline) {
      write('{');
      for (size_t i = 0; i < lit->fields.size(); i++) {
        if (i > 0)
          write(", ");
        write(lit->fields[i].name.id_value);
        write(" = ");
        format_expr(lit->fields[i].value);
      }
      write('}');
      return;
    }

    size_t max_name = 0;
    for (const auto &field: lit->fields)
      max_name = std::max(max_name, field.name.id_value.size());

    write('{');
    newline();
    _indent++;
    for (size_t i = 0; i < lit->fields.size(); i++) {
      begin_line();
      write(lit->fields[i].name.id_value);
      write_padding(_indent * 2 + max_name + 1);
      write("= ");
      format_expr(lit->fields[i].value);
      write(',');
      newline();
    }
    _indent--;
    begin_line();
    write('}');
  }

  void format_block_stmts(const std::vector<Stmt *> &stmts) {
    write('{');
    newline();
    _indent++;
    format_stmts(stmts);
    _indent--;
    begin_line();
    write('}');
    newline();
  }

  void format_stmts(const std::vector<Stmt *> &stmts) {
    std::vector<StmtGroup> groups = partition_stmt_groups(stmts);
    for (size_t g = 0; g < groups.size(); g++) {
      if (g > 0) {
        bool blank = should_blank_between_stmt_groups(groups[g - 1], groups[g]);
        if (!blank && !groups[g - 1].stmts.empty() && !groups[g].stmts.empty()) {
          Stmt *prev = groups[g - 1].stmts.back();
          Stmt *next = groups[g].stmts.front();
          if (source_has_blank_gap(_source, prev->end, next->start))
            blank = true;
        }
        if (blank)
          newline();
      }
      format_stmt_group(groups[g]);
    }
  }

  void format_stmt_group(const StmtGroup &group) {
    switch (group.kind) {
    case StmtGroupKind::Vars:
      format_var_group(group.stmts);
      break;
    case StmtGroupKind::FieldAssigns:
      format_field_assign_group(group.stmts);
      break;
    case StmtGroupKind::Assigns:
    case StmtGroupKind::Exprs:
    case StmtGroupKind::BreakContinue:
    case StmtGroupKind::Returns:
      for (Stmt *stmt: group.stmts)
        format_stmt(stmt);
      break;
    case StmtGroupKind::Control:
    case StmtGroupKind::Comptime:
    case StmtGroupKind::Singleton:
      format_stmt(group.stmts.front());
      break;
    }
  }

  void format_field_assign_group(const std::vector<Stmt *> &stmts) {
    size_t max_lhs = 0;
    for (Stmt *stmt: stmts) {
      auto *assign = std::get<stmt::Assign *>(stmt->data);
      max_lhs = std::max(max_lhs, assign_lhs_text(assign->target).size());
    }
    for (Stmt *stmt: stmts) {
      auto *assign = std::get<stmt::Assign *>(stmt->data);
      emit_comments_until(stmt->start);
      begin_line();
      format_expr(assign->target);
      write_padding(_indent * 2 + max_lhs + 1);
      write("= ");
      format_expr(assign->value);
      write(';');
      newline();
    }
  }

  void format_compact_stmt_body(Stmt *stmt) {
    switch (stmt->type) {
    case STMT_RETURN: {
      auto *ret = std::get<stmt::Return *>(stmt->data);
      write("return");
      if (ret->value.has_value()) {
        write(' ');
        format_expr(ret->value.value());
      }
      write(';');
      break;
    }
    case STMT_ASSIGN: {
      auto *assign = std::get<stmt::Assign *>(stmt->data);
      format_expr(assign->target);
      write(" = ");
      format_expr(assign->value);
      write(';');
      break;
    }
    case STMT_EXPR: {
      format_expr(std::get<stmt::ExprStmt *>(stmt->data)->expr);
      write(';');
      break;
    }
    case STMT_BREAK:
      write("break;");
      break;
    case STMT_CONTINUE:
      write("continue;");
      break;
    default:
      break;
    }
  }

  void format_if_chain_compact(stmt::If *if_) {
    begin_line();
    bool first = true;
    for (;;) {
      write(first ? "if " : " else if ");
      first = false;
      format_expr(if_->cond);
      write(" { ");
      format_compact_stmt_body(if_->then_block.front());
      write(" }");

      if (if_->else_block.empty())
        break;

      if (if_->else_block.size() == 1 &&
          if_->else_block.front()->type == STMT_IF) {
        if_ = std::get<stmt::If *>(if_->else_block.front()->data);
        continue;
      }

      write(" else { ");
      format_compact_stmt_body(if_->else_block.front());
      write(" }");
      break;
    }
    newline();
  }

  void format_var_group(const std::vector<Stmt *> &stmts) {
    std::vector<VarDeclView> views = flatten_var_stmts(stmts);
    size_t i = 0;
    while (i < views.size()) {
      stmt::Var *first = views[i].var;

      if (first->type != nullptr && !first->value.has_value()) {
        size_t j = i + 1;
        while (j < views.size() && views[j].var->type != nullptr &&
               !views[j].var->value.has_value() &&
               types_equal(views[j].var->type, first->type))
          j++;
        if (j - i >= 2) {
          emit_comments_until(views[i].stmt->start);
          begin_line();
          write("var ");
          for (size_t k = i; k < j; k++) {
            write(views[k].var->name.id_value);
            if (k + 1 < j)
              write(", ");
          }
          write(' ');
          format_type(first->type);
          write(';');
          newline();
          i = j;
          continue;
        }
      }

      if (first->type != nullptr && first->value.has_value() &&
          is_simple_init(first->value.value(), _source)) {
        size_t j = i + 1;
        while (j < views.size() &&
               can_share_var_line(views[j - 1].var, views[j].var, _source))
          j++;
        if (j - i >= 2) {
          emit_comments_until(views[i].stmt->start);
          begin_line();
          write("var ");
          for (size_t k = i; k < j; k++) {
            auto *var = views[k].var;
            write(var->name.id_value);
            write(' ');
            format_type(var->type);
            write(" = ");
            format_expr(var->value.value());
            if (k + 1 < j)
              write(", ");
          }
          write(';');
          newline();
          i = j;
          continue;
        }
      }

      format_stmt(views[i].stmt);
      i++;
    }
  }

  void format_stmt(Stmt *stmt) {
    if (stmt == nullptr)
      return;
    emit_comments_until(stmt->start);
    switch (stmt->type) {
    case STMT_BLOCK: {
      auto *block = std::get<stmt::Block *>(stmt->data);
      if (block->scoped)
        format_block_stmts(block->stmts);
      else
        format_stmts(block->stmts);
      break;
    }
    case STMT_VAR: {
      auto *var = std::get<stmt::Var *>(stmt->data);
      begin_line();
      write("var ");
      write(var->name.id_value);
      if (var->type != nullptr) {
        write(' ');
        format_type(var->type);
      }
      if (var->value.has_value()) {
        write(" = ");
        format_expr(var->value.value());
      }
      write(';');
      newline();
      break;
    }
    case STMT_RETURN: {
      auto *ret = std::get<stmt::Return *>(stmt->data);
      begin_line();
      write("return");
      if (ret->value.has_value()) {
        write(' ');
        format_expr(ret->value.value());
      }
      write(';');
      newline();
      break;
    }
    case STMT_ASSIGN: {
      auto *assign = std::get<stmt::Assign *>(stmt->data);
      begin_line();
      format_expr(assign->target);
      write(" = ");
      format_expr(assign->value);
      write(';');
      newline();
      break;
    }
    case STMT_WHILE: {
      auto *while_ = std::get<stmt::While *>(stmt->data);
      begin_line();
      write("while ");
      format_expr(while_->cond);
      write(' ');
      format_block_stmts(while_->body);
      break;
    }
    case STMT_FOR: {
      auto *for_ = std::get<stmt::For *>(stmt->data);
      begin_line();
      write("for ");
      format_stmt_inline(for_->init);
      write(';');
      write(' ');
      format_expr(for_->cond);
      write(';');
      write(' ');
      format_stmt_inline(for_->step);
      write(' ');
      write('{');
      newline();
      _indent++;
      format_stmts(for_->body);
      _indent--;
      begin_line();
      write('}');
      newline();
      break;
    }
    case STMT_FOR_IN: {
      auto *for_in = std::get<stmt::ForIn *>(stmt->data);
      begin_line();
      write("for var ");
      write(for_in->binding->name.id_value);
      if (for_in->binding->type != nullptr) {
        write(' ');
        format_type(for_in->binding->type);
      }
      write(" in ");
      format_expr(for_in->iterable);
      write(' ');
      format_block_stmts(for_in->body);
      break;
    }
    case STMT_IF:
      format_if_stmt(stmt);
      break;
    case STMT_WHEN:
      format_when_stmt(stmt);
      break;
    case STMT_BREAK:
      begin_line();
      write("break;");
      newline();
      break;
    case STMT_CONTINUE:
      begin_line();
      write("continue;");
      newline();
      break;
    case STMT_EXPR: {
      auto *expr_stmt = std::get<stmt::ExprStmt *>(stmt->data);
      begin_line();
      format_expr(expr_stmt->expr);
      write(';');
      newline();
      break;
    }
    case STMT_COMPTIME_BLOCK:
      format_comptime_block(stmt->start, std::get<decl::ComptimeBlock *>(stmt->data));
      break;
    }
  }

  void format_stmt_inline(Stmt *stmt) {
    if (stmt == nullptr)
      return;
    emit_comments_until(stmt->start);
    switch (stmt->type) {
    case STMT_VAR: {
      auto *var = std::get<stmt::Var *>(stmt->data);
      write("var ");
      write(var->name.id_value);
      if (var->type != nullptr) {
        write(' ');
        format_type(var->type);
      }
      if (var->value.has_value()) {
        write(" = ");
        format_expr(var->value.value());
      }
      break;
    }
    case STMT_ASSIGN: {
      auto *assign = std::get<stmt::Assign *>(stmt->data);
      format_expr(assign->target);
      write(" = ");
      format_expr(assign->value);
      break;
    }
    default:
      format_stmt(stmt);
      break;
    }
  }

  void format_if_stmt(Stmt *stmt) {
    auto *if_ = std::get<stmt::If *>(stmt->data);
    if (is_compact_if_chain(if_, _source)) {
      format_if_chain_compact(if_);
      return;
    }
    begin_line();
    write("if ");
    format_expr(if_->cond);
    write(' ');
    write('{');
    newline();
    _indent++;
    format_stmts(if_->then_block);
    _indent--;
    if (!if_->else_block.empty()) {
      if (if_->else_block.size() == 1 && if_->else_block.front()->type == STMT_IF) {
        begin_line();
        write("} else if ");
        auto *else_if = std::get<stmt::If *>(if_->else_block.front()->data);
        format_expr(else_if->cond);
        write(' ');
        write('{');
        newline();
        _indent++;
        format_stmts(else_if->then_block);
        _indent--;
        if (!else_if->else_block.empty()) {
          format_else_tail(else_if->else_block);
        } else {
          begin_line();
          write('}');
          newline();
        }
      } else {
        begin_line();
        write("} else {");
        newline();
        _indent++;
        format_stmts(if_->else_block);
        _indent--;
        begin_line();
        write('}');
        newline();
      }
    } else {
      begin_line();
      write('}');
      newline();
    }
  }

  void format_else_tail(const std::vector<Stmt *> &else_block) {
    if (else_block.size() == 1 && else_block.front()->type == STMT_IF) {
      begin_line();
      write("} else if ");
      auto *else_if = std::get<stmt::If *>(else_block.front()->data);
      format_expr(else_if->cond);
      write(' ');
      write('{');
      newline();
      _indent++;
      format_stmts(else_if->then_block);
      _indent--;
      if (!else_if->else_block.empty())
        format_else_tail(else_if->else_block);
      else {
        begin_line();
        write('}');
        newline();
      }
    } else {
      begin_line();
      write("} else {");
      newline();
      _indent++;
      format_stmts(else_block);
      _indent--;
      begin_line();
      write('}');
      newline();
    }
  }

  void format_when_stmt(Stmt *stmt) {
    auto *when = std::get<stmt::When *>(stmt->data);
    begin_line();
    write("when ");
    format_expr(when->cond);
    write(' ');
    format_block_stmts(when->true_block);
    if (!when->false_block.empty()) {
      begin_line();
      write("else ");
      format_block_stmts(when->false_block);
    }
  }

  void format_comptime_block(size_t start, decl::ComptimeBlock *block) {
    emit_comments_until(start);
    begin_line();
    write("comptime {");
    newline();
    _indent++;
    std::vector<DeclGroup> decl_groups = partition_decl_groups(block->decls);
    for (size_t g = 0; g < decl_groups.size(); g++) {
      for (Decl *decl: decl_groups[g].decls)
        format_decl(decl);
      if (g + 1 < decl_groups.size() &&
          should_blank_between_decl_groups(decl_groups[g].kind,
                                           decl_groups[g + 1].kind))
        newline();
    }
    format_stmts(block->stmts);
    _indent--;
    begin_line();
    write('}');
    newline();
  }

  void format_decl(Decl *decl) {
    if (decl == nullptr)
      return;
    emit_comments_until(decl->start);
    switch (decl->type) {
    case DECL_CONST: {
      auto *konst = std::get<decl::Const *>(decl->data);
      begin_line();
      write("const ");
      write(konst->name.id_value);
      write(' ');
      format_type(konst->type);
      write(" = ");
      format_expr(konst->value);
      write(';');
      newline();
      break;
    }
    case DECL_PROC: {
      auto *proc = std::get<decl::Proc *>(decl->data);
      begin_line();
      if (proc->is_comptime)
        write("comptime ");
      if (proc->linkage == LINK_EXTERN)
        write("extern ");
      write("proc ");
      write(proc->name.id_value);
      write('(');
      std::vector<std::pair<std::string_view, Type *>> params{};
      for (const Param &param: proc->params)
        params.emplace_back(param.name.id_value, param.type);
      write_name_type_groups(params);
      write(") ");
      if (proc->ret_type.has_value())
        format_type(*proc->ret_type);
      if (proc->linkage == LINK_EXTERN) {
        write(';');
        newline();
      } else {
        write(' ');
        format_block_stmts(proc->body);
      }
      break;
    }
    case DECL_WHEN: {
      auto *when = std::get<decl::When *>(decl->data);
      begin_line();
      write("when ");
      format_expr(when->cond);
      write(' ');
      write('{');
      newline();
      _indent++;
      for (Decl *inner: when->true_block)
        format_decl(inner);
      _indent--;
      if (!when->false_block.empty()) {
        begin_line();
        write("} else {");
        newline();
        _indent++;
        for (Decl *inner: when->false_block)
          format_decl(inner);
        _indent--;
      }
      begin_line();
      write('}');
      newline();
      break;
    }
    case DECL_IMPORT: {
      auto *import = std::get<decl::Import *>(decl->data);
      begin_line();
      write("import ");
      write_source_span(import->path.start, import->path.end);
      write(';');
      newline();
      break;
    }
    case DECL_COMPTIME_BLOCK:
      format_comptime_block(decl->start,
                            std::get<decl::ComptimeBlock *>(decl->data));
      break;
    case DECL_STRUCT: {
      auto *strukt = std::get<decl::Struct *>(decl->data);
      begin_line();
      write("struct ");
      write(strukt->name.id_value);
      write(' ');
      write('{');
      newline();
      _indent++;
      std::vector<std::pair<std::string_view, Type *>> fields{};
      for (const decl::StructField &field: strukt->fields)
        fields.emplace_back(field.name.id_value, field.type);
      size_t i = 0;
      while (i < fields.size()) {
        size_t j = i + 1;
        while (j < fields.size() && types_equal(fields[j].second, fields[i].second))
          j++;
        begin_line();
        for (size_t k = i; k < j; k++) {
          write(fields[k].first);
          if (k + 1 < j)
            write(", ");
        }
        write(' ');
        format_type(fields[i].second);
        write(';');
        newline();
        i = j;
      }
      _indent--;
      begin_line();
      write('}');
      newline();
      break;
    }
    case DECL_ENUM: {
      auto *enum_ = std::get<decl::Enum *>(decl->data);
      begin_line();
      write("enum ");
      write(enum_->name.id_value);
      if (enum_->underlying != nullptr) {
        write(" : ");
        format_type(enum_->underlying);
      }
      write(' ');
      write('{');
      newline();
      _indent++;
      size_t max_name = 0;
      bool any_explicit = false;
      for (const decl::EnumMember &member: enum_->members) {
        max_name = std::max(max_name, member.name.id_value.size());
        if (member.value.has_value())
          any_explicit = true;
      }
      for (size_t i = 0; i < enum_->members.size(); i++) {
        const decl::EnumMember &member = enum_->members[i];
        begin_line();
        write(member.name.id_value);
        if (member.value.has_value()) {
          if (any_explicit)
            write_padding(_indent * 2 + max_name + 1);
          write("= ");
          format_expr(member.value.value());
        }
        write(',');
        newline();
      }
      _indent--;
      begin_line();
      write('}');
      newline();
      break;
    }
    }
  }
};

} // namespace

std::optional<std::string> format_rye_source(std::string_view source) {
  std::string source_copy(source);
  Lexer lexer(source_copy);
  auto tokens = lexer.lex_tokens();
  if (!tokens.has_value())
    return std::nullopt;

  Arena arena{};
  Parser parser(tokens.value(), arena);
  auto decls = parser.parse_decls();
  if (!decls.has_value())
    return std::nullopt;

  std::vector<Comment> comments = collect_comments(source);
  RyeFormatter formatter(source, comments);
  return formatter.format_decls(decls.value());
}
