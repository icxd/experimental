#include <algorithm>
#include <utility>

#include <checker/checker.hpp>
#include <host.hpp>
#include <module.hpp>
#include "parser/ast.hpp"

namespace fs = std::filesystem;

ErrorOr<void> Checker::inject_builtin_consts() {
  _consts.push_back(CheckedConst{"OS_LINUX", new Type{TYPE_INT},
                                 new Expr{EXPR_INT, 0, 0, new expr::Int{0}}});
  _consts.push_back(CheckedConst{"OS_MACOS", new Type{TYPE_INT},
                                 new Expr{EXPR_INT, 0, 0, new expr::Int{1}}});
  _consts.push_back(CheckedConst{"TARGET_OS", new Type{TYPE_INT},
                                 new Expr{EXPR_INT, 0, 0,
                                          new expr::Int{_config.target_os}}});
  _consts.push_back(CheckedConst{"ARCH_X86_64", new Type{TYPE_INT},
                                 new Expr{EXPR_INT, 0, 0, new expr::Int{0}}});
  _consts.push_back(CheckedConst{"ARCH_AARCH64", new Type{TYPE_INT},
                                 new Expr{EXPR_INT, 0, 0, new expr::Int{1}}});
  _consts.push_back(CheckedConst{"TARGET_ARCH", new Type{TYPE_INT},
                                 new Expr{EXPR_INT, 0, 0,
                                          new expr::Int{_config.target_arch}}});
  return {};
}

ErrorOr<void> Checker::inject_define_consts() {
  for (const auto &[name, value]: _config.defines) {
    if (find_const(name).has_value()) {
      return std::unexpected(
          Error(std::format("Cannot redefine constant `{}`", name), 0, 0)
              .with_hint("`-D` conflicts with an existing constant"));
    }

    if (value == "true" || value == "false") {
      bool bool_value = value == "true";
      _consts.push_back(CheckedConst{
          name,
          new Type{TYPE_BOOL},
          new Expr{EXPR_BOOL, 0, 0, new expr::Bool{bool_value}},
      });
      continue;
    }

    try {
      size_t consumed = 0;
      int64_t int_value = std::stoll(value, &consumed);
      if (consumed != value.size()) {
        return std::unexpected(
            Error(std::format("Invalid compile-time define `{}={}`", name, value),
                  0, 0)
                .with_hint("Use `true`, `false`, or an integer literal"));
      }
      _consts.push_back(CheckedConst{
          name,
          new Type{TYPE_INT},
          new Expr{EXPR_INT, 0, 0, new expr::Int{int_value}},
      });
    } catch (const std::exception &) {
      return std::unexpected(
          Error(std::format("Invalid compile-time define `{}={}`", name, value),
                0, 0)
              .with_hint("Use `true`, `false`, or an integer literal"));
    }
  }

  return {};
}

ErrorOr<std::vector<Decl *>>
Checker::resolve_when_branch(const std::vector<Decl *> &block, Scope *scope) {
  std::vector<Decl *> result{};
  for (Decl *decl: block) {
    if (decl->type == DECL_WHEN) {
      std::vector<Decl *> nested = try$(resolve_when_decl(decl, scope));
      result.insert(result.end(), nested.begin(), nested.end());
    } else {
      result.push_back(decl);
    }
  }
  return result;
}

ErrorOr<std::vector<Decl *>> Checker::resolve_when_decl(Decl *decl,
                                                        Scope *scope) {
  decl::When *when = std::get<decl::When *>(decl->data);

  Expr *cond = try$(evaluate_constant(when->cond, scope));
  when->cond = cond;

  Type *cond_type = try$(check_expr(cond, scope));
  if (!type_eq(cond_type, new Type{TYPE_BOOL})) {
    return std::unexpected(
        Error("Condition has to be a boolean", cond->start, cond->end)
            .with_hint(
                std::format("Expected `bool`, found `{}`", cond_type->to_string())));
  }

  when->cond->expr_type = new Type{TYPE_BOOL, cond_type->start, cond_type->end};

  bool value = std::get<expr::Bool *>(cond->data)->value;
  const std::vector<Decl *> &block =
      value ? when->true_block : when->false_block;
  if (block.empty()) {
    return std::unexpected(
        Error("Empty branch in `when` declaration", decl->start, decl->end)
            .with_hint(
                "Expected at least one branch to contain executable code"));
  }

  return resolve_when_branch(block, scope);
}

ErrorOr<std::vector<Stmt *>>
Checker::resolve_when_branch_stmts(const std::vector<Stmt *> &block,
                                   Scope *scope) {
  std::vector<Stmt *> result{};
  for (Stmt *stmt: block) {
    if (stmt->type == STMT_WHEN) {
      std::vector<Stmt *> nested = try$(resolve_when_stmt(stmt, scope));
      result.insert(result.end(), nested.begin(), nested.end());
    } else {
      result.push_back(stmt);
    }
  }
  return result;
}

ErrorOr<std::vector<Stmt *>> Checker::resolve_when_stmt(Stmt *stmt,
                                                        Scope *scope) {
  stmt::When *when = std::get<stmt::When *>(stmt->data);

  Expr *cond = try$(evaluate_constant(when->cond, scope));
  when->cond = cond;

  Type *cond_type = try$(check_expr(cond, scope));
  if (!type_eq(cond_type, new Type{TYPE_BOOL})) {
    return std::unexpected(
        Error("Condition has to be a boolean", cond->start, cond->end)
            .with_hint(
                std::format("Expected `bool`, found `{}`", cond_type->to_string())));
  }

  when->cond->expr_type = new Type{TYPE_BOOL, cond_type->start, cond_type->end};

  bool value = std::get<expr::Bool *>(cond->data)->value;
  const std::vector<Stmt *> &block =
      value ? when->true_block : when->false_block;

  return resolve_when_branch_stmts(block, scope);
}

ErrorOr<void> Checker::expand_when_stmts(std::vector<Stmt *> &stmts,
                                          Scope *scope) {
  for (size_t i = 0; i < stmts.size();) {
    Stmt *stmt = stmts[i];
    if (stmt->type == STMT_WHEN) {
      std::vector<Stmt *> expanded = try$(resolve_when_stmt(stmt, scope));
      stmts.erase(stmts.begin() + static_cast<std::ptrdiff_t>(i));
      stmts.insert(stmts.begin() + static_cast<std::ptrdiff_t>(i),
                   expanded.begin(), expanded.end());
      continue;
    }

    if (stmt->type == STMT_BLOCK) {
      stmt::Block *block = std::get<stmt::Block *>(stmt->data);
      try$(expand_when_stmts(block->stmts, scope));
    }

    ++i;
  }

  return {};
}

ErrorOr<void> Checker::check_decls(std::vector<Decl *> &decls) {
  try$(inject_builtin_consts());
  try$(inject_define_consts());

  for (size_t i = 0; i < decls.size();) {
    Decl *decl = decls[i];
    if (decl->type == DECL_WHEN) {
      std::vector<Decl *> expanded = try$(resolve_when_decl(decl, _global_scope));
      decls.erase(decls.begin() + static_cast<std::ptrdiff_t>(i));
      decls.insert(decls.begin() + static_cast<std::ptrdiff_t>(i),
                   expanded.begin(), expanded.end());
      continue;
    }

    if (decl->type == DECL_COMPTIME_BLOCK) {
      decl::ComptimeBlock *block =
          std::get<decl::ComptimeBlock *>(decl->data);
      decls.erase(decls.begin() + static_cast<std::ptrdiff_t>(i));
      decls.insert(decls.begin() + static_cast<std::ptrdiff_t>(i),
                   block->decls.begin(), block->decls.end());
      continue;
    }

    try$(check_decl(decl, _global_scope));
    ++i;
  }

  return {};
}

ErrorOr<void> Checker::check_decl(Decl *decl, Scope *scope) {
  switch (decl->type) {
  case DECL_CONST: {
    decl::Const *const_ = std::get<decl::Const *>(decl->data);
    if (find_const(const_->name.id_value).has_value()) {
      return std::unexpected(Error(
          std::format("Redeclaration of constant `{}`", const_->name.id_value),
          decl->start, decl->end));
    }
    Type *type = try$(check_type(const_->type, scope));
    Expr *value = try$(evaluate_constant(const_->value, scope));
    const_->value = value;
    Type *value_type = try$(check_expr(value, scope));
    if (!type_eq(type, value_type)) {
      return std::unexpected(
          Error(std::format("Constant `{}` has type `{}`, but value has type "
                            "`{}`",
                            const_->name.id_value, type->to_string(),
                            value_type->to_string()),
                decl->start, decl->end)
              .with_hint(std::format("Expected `{}`, found `{}`",
                                     type->to_string(),
                                     value_type->to_string())));
    }
    const_->value->expr_type = type;
    value->expr_type = type;
    _consts.push_back(CheckedConst{
        const_->name.id_value,
        type,
        value,
    });

    return {};
  }

  case DECL_PROC: {
    decl::Proc *proc = std::get<decl::Proc *>(decl->data);
    if (proc->is_comptime)
      return check_comptime_proc(proc, scope);

    if (find_local_proc(proc->name.id_value).has_value() ||
        find_comptime_proc(proc->name.id_value).has_value()) {
      return std::unexpected(Error(
          std::format("Redeclaration of procedure `{}`", proc->name.id_value),
          decl->start, decl->end));
    }

    Scope *proc_scope = Scope::create(scope);

    std::set<std::string_view> param_names{};
    std::vector<CheckedParam> params{};
    for (const Param &param: proc->params) {
      std::string_view name = param.name.id_value;
      if (param_names.contains(name)) {
        return std::unexpected(
            Error(std::format("Parameter with name `{}` already exists", name),
                  param.name.start, param.name.end));
      }

      Type *type = try$(check_type(param.type, scope));

      param_names.insert(name);
      params.push_back({name, type});
      proc_scope->vars.insert({name, type});
    }

    Type *ret_type = new Type{TYPE_VOID, proc->name.start, proc->name.end};
    if (proc->ret_type.has_value()) {
      Type *type = proc->ret_type.value();
      ret_type = try$(check_type(type, proc_scope));
    }

    _procs.push_back(CheckedProc{
        proc->name.id_value,
        params,
        ret_type,
        proc_scope,
    });
    _current_proc_id = _procs.size() - 1;

    try$(expand_when_stmts(proc->body, proc_scope));
    for (auto *stmt: proc->body)
      try$(check_stmt(stmt, proc_scope));

    _current_proc_id = std::numeric_limits<size_t>::max();

    return {};
  }

  case DECL_WHEN:
  case DECL_COMPTIME_BLOCK:
    PANIC("unreachable");

  case DECL_IMPORT: {
    decl::Import *import = std::get<decl::Import *>(decl->data);
    return check_import(import, decl->start, decl->end);
  }
  }
  std::unreachable();
}

ErrorOr<void> Checker::check_import(decl::Import *import, size_t start,
                                    size_t end) {
  std::vector<fs::path> search_paths{};
  for (const auto &path: _import_search_paths)
    search_paths.push_back(fs::path(path));

  auto resolved =
      resolve_import_path(_file_path, import->path.string_value, search_paths);
  if (!resolved.has_value()) {
    return std::unexpected(
        Error(std::format("Could not find module `{}`", import->path.string_value),
              start, end)
            .with_hint("Paths are resolved relative to the importing file, "
                       "then along each `-I` search path"));
  }

  std::string import_module = module_name_from_path(*resolved);
  if (_registry == nullptr || !_registry->has_module(import_module)) {
    return std::unexpected(
        Error(std::format("Module `{}` is not available (import `{}` resolves "
                           "to `{}`)",
                           import_module, import->path.string_value,
                           resolved->string()),
              start, end)
            .with_hint("Imported modules must be compiled before the importer"));
  }

  if (std::find(_imports.begin(), _imports.end(), import_module) ==
      _imports.end())
    _imports.push_back(import_module);

  return {};
}

ErrorOr<void> Checker::check_stmt(Stmt *stmt, Scope *scope) {
  switch (stmt->type) {
  case STMT_BLOCK: {
    stmt::Block *block = std::get<stmt::Block *>(stmt->data);
    try$(expand_when_stmts(block->stmts, scope));
    for (Stmt *inner: block->stmts)
      try$(check_stmt(inner, scope));
    return {};
  }

  case STMT_WHEN:
    PANIC("unreachable");

  case STMT_ASSIGN: {
    stmt::Assign *assign = std::get<stmt::Assign *>(stmt->data);
    std::string_view name = assign->name.id_value;
    auto found = scope->find_var(name);
    if (!found.has_value()) {
      return std::unexpected(
          Error(std::format("Cannot assign to undeclared variable `{}`", name),
                assign->name.start, assign->name.end));
    }
    Type *var_type = found.value();
    Type *expr_type = try$(check_expr(assign->value, scope));
    if (!type_eq(var_type, expr_type)) {
      return std::unexpected(
          Error(std::format(
                    "Cannot assign value of type `{}` to variable of type `{}`",
                    expr_type->to_string(), var_type->to_string()),
                assign->value->start, assign->value->end)
              .with_hint(std::format("Expected `{}`, found `{}`",
                                     var_type->to_string(),
                                     expr_type->to_string())));
    }
    assign->value->expr_type = var_type;
    return {};
  }

  case STMT_WHILE: {
    stmt::While *while_ = std::get<stmt::While *>(stmt->data);
    Type *cond_type = try$(check_expr(while_->cond, scope));
    if (!type_eq(cond_type, new Type{TYPE_BOOL})) {
      return std::unexpected(
          Error("Condition has to be a boolean", while_->cond->start,
                while_->cond->end)
              .with_hint(std::format("Expected `bool`, found `{}`",
                                     cond_type->to_string())));
    }
    while_->cond->expr_type =
        new Type{TYPE_BOOL, while_->cond->start, while_->cond->end};
    _loop_depth++;
    for (Stmt *inner: while_->body)
      try$(check_stmt(inner, scope));
    _loop_depth--;
    return {};
  }

  case STMT_FOR: {
    stmt::For *for_ = std::get<stmt::For *>(stmt->data);
    if (for_->init->type != STMT_VAR && for_->init->type != STMT_ASSIGN) {
      return std::unexpected(
          Error("For-loop init must be a variable declaration or assignment",
                for_->init->start, for_->init->end));
    }
    if (for_->step->type != STMT_ASSIGN) {
      return std::unexpected(
          Error("For-loop step must be an assignment", for_->step->start,
                for_->step->end));
    }
    try$(check_stmt(for_->init, scope));
    Type *cond_type = try$(check_expr(for_->cond, scope));
    if (!type_eq(cond_type, new Type{TYPE_BOOL})) {
      return std::unexpected(
          Error("Condition has to be a boolean", for_->cond->start,
                for_->cond->end)
              .with_hint(std::format("Expected `bool`, found `{}`",
                                     cond_type->to_string())));
    }
    for_->cond->expr_type =
        new Type{TYPE_BOOL, for_->cond->start, for_->cond->end};
    _loop_depth++;
    for (Stmt *inner: for_->body)
      try$(check_stmt(inner, scope));
    _loop_depth--;
    try$(check_stmt(for_->step, scope));
    return {};
  }

  case STMT_IF: {
    stmt::If *if_ = std::get<stmt::If *>(stmt->data);
    Type *cond_type = try$(check_expr(if_->cond, scope));
    if (!type_eq(cond_type, new Type{TYPE_BOOL})) {
      return std::unexpected(
          Error("Condition has to be a boolean", if_->cond->start,
                if_->cond->end)
              .with_hint(std::format("Expected `bool`, found `{}`",
                                     cond_type->to_string())));
    }
    if_->cond->expr_type = new Type{TYPE_BOOL, if_->cond->start, if_->cond->end};
    for (Stmt *inner: if_->then_block)
      try$(check_stmt(inner, scope));
    for (Stmt *inner: if_->else_block)
      try$(check_stmt(inner, scope));
    return {};
  }

  case STMT_VAR: {
    stmt::Var *var = std::get<stmt::Var *>(stmt->data);

    std::string_view name = var->name.id_value;
    Type *type = try$(check_type(var->type, scope));
    Type *expr_type = try$(check_expr(var->value, scope));

    if (!type_eq(type, expr_type)) {
      return std::unexpected(
          Error(std::format(
                    "Cannot assign value of type `{}` to variable of type `{}`",
                    expr_type->to_string(), type->to_string()),
                var->value->start, var->value->end)
              .with_hint(std::format("Expected `{}`, found `{}`",
                                     type->to_string(),
                                     expr_type->to_string())));
    }

    var->value->expr_type = type;
    scope->vars.insert({name, type});

    return {};
  }

  case STMT_RETURN: {
    stmt::Return *ret = std::get<stmt::Return *>(stmt->data);

    CheckedProc curr_proc = _procs.at(_current_proc_id);
    if (curr_proc.ret_type->type == TYPE_VOID && ret->value.has_value()) {
      return std::unexpected(
          Error("Cannot return a value from a function with return type `void`",
                ret->value.value()->start, ret->value.value()->end)
              .with_hint("This function is declared to return `void`"));
    }

    if (curr_proc.ret_type->type != TYPE_VOID && !ret->value.has_value()) {
      return std::unexpected(
          Error(std::format("Missing return value in function returning `{}`",
                            curr_proc.ret_type->to_string()),
                ret->value.value()->start, ret->value.value()->end)
              .with_hint(
                  std::format("Expected a value of type `{}` to be returned",
                              curr_proc.ret_type->to_string())));
    }

    if (ret->value.has_value()) {
      Expr *value = ret->value.value();
      Type *type = try$(check_expr(value, scope));
      if (!type_eq(curr_proc.ret_type, type)) {
        auto error = Error("Return type mismatch", ret->value.value()->start,
                           ret->value.value()->end)
                         .with_hint(std::format("Expected `{}`, found `{}`",
                                                curr_proc.ret_type->to_string(),
                                                type->to_string()));
        auto fix = suggest_fix(curr_proc.ret_type, type, value);
        if (fix.has_value())
          error.add_help(ErrorHelp(*fix, value->start, value->end));
        return std::unexpected(error);
      }

      ret->value.value()->expr_type = type;
    }

    return {};
  }

  case STMT_BREAK: {
    if (_loop_depth == 0) {
      return std::unexpected(
          Error("`break` outside of loop", stmt->start, stmt->end));
    }
    return {};
  }

  case STMT_CONTINUE: {
    if (_loop_depth == 0) {
      return std::unexpected(
          Error("`continue` outside of loop", stmt->start, stmt->end));
    }
    return {};
  }
  }
  std::unreachable();
}

ErrorOr<Type *> Checker::check_expr(Expr *expr, Scope *scope) {
  if (expr->type == EXPR_COMPTIME_CALL) {
    Expr *folded = try$(evaluate_constant(expr, scope, nullptr));
    expr->type = folded->type;
    expr->data = folded->data;
    if (folded->expr_type != nullptr)
      expr->expr_type = folded->expr_type;
    return check_expr(expr, scope);
  }

  Type *type = nullptr;

  switch (expr->type) {
  case EXPR_INT:
    type = new Type{TYPE_INT, expr->start, expr->end};
    break;

  case EXPR_BOOL:
    type = new Type{TYPE_BOOL, expr->start, expr->end};
    break;

  case EXPR_VAR: {
    expr::Var *var = std::get<expr::Var *>(expr->data);

    if (var->module.has_value()) {
      auto const_ =
          find_qualified_const(var->module->id_value, var->var.id_value);
      if (!const_.has_value()) {
        return std::unexpected(
            Error(std::format("Use undeclared constant `{}:{}`",
                              var->module->id_value, var->var.id_value),
                  expr->start, expr->end));
      }
      type = const_->type;
      break;
    }

    for (const auto &local: _consts) {
      if (local.name == var->var.id_value) {
        type = local.type;
        break;
      }
    }
    if (type != nullptr)
      break;

    auto found = scope->find_var(var->var.id_value);
    if (found.has_value()) {
      type = found.value();
      break;
    }

    if (!_imports.empty()) {
      CheckedConst imported =
          try$(find_imported_const(var->var.id_value, expr->start, expr->end));
      type = imported.type;
      break;
    }

    return std::unexpected(
        Error(std::format("Use undeclared variable `{}`", var->var.id_value),
              expr->start, expr->end));
  }

  case EXPR_GROUP: {
    expr::Group *group = std::get<expr::Group *>(expr->data);
    type = try$(check_expr(group->expr, scope));
    group->expr->expr_type = type;
    break;
  }

  case EXPR_BINARY: {
    expr::Binary *binary = std::get<expr::Binary *>(expr->data);

    Type *lhs_type = try$(check_expr(binary->lhs, scope));
    binary->lhs->expr_type = lhs_type;

    Type *rhs_type = try$(check_expr(binary->rhs, scope));
    binary->rhs->expr_type = rhs_type;

    if (!type_eq(lhs_type, rhs_type)) {
      return std::unexpected(
          Error("Invalid binary operation", expr->start, expr->end)
              .with_hint(std::format("Cannot apply operator `{}` to operands "
                                     "of type `{}` and `{}`",
                                     binop_to_string(binary->op),
                                     lhs_type->to_string(),
                                     rhs_type->to_string())));
    }

    switch (binary->op) {
    case expr::Binary::BINOP_PLUS:
    case expr::Binary::BINOP_MINUS:
    case expr::Binary::BINOP_STAR:
    case expr::Binary::BINOP_SLASH:
      type = lhs_type;
      break;

    case expr::Binary::BINOP_EQ:
    case expr::Binary::BINOP_NEQ:
    case expr::Binary::BINOP_LT:
    case expr::Binary::BINOP_LTE:
    case expr::Binary::BINOP_GT:
    case expr::Binary::BINOP_GTE:
      type = new Type{TYPE_BOOL, expr->start, expr->end};
      break;

    case expr::Binary::BINOP_AND:
    case expr::Binary::BINOP_OR: {
      if (!type_eq(lhs_type, new Type{TYPE_BOOL}) ||
          !type_eq(rhs_type, new Type{TYPE_BOOL})) {
        return std::unexpected(
            Error("Logical operators require boolean operands", expr->start,
                  expr->end)
                .with_hint(std::format(
                    "Expected `bool`, found `{}` and `{}`",
                    lhs_type->to_string(), rhs_type->to_string())));
      }
      type = new Type{TYPE_BOOL, expr->start, expr->end};
      break;
    }
    }
    break;
  }

  case EXPR_NOT: {
    expr::Not *not_ = std::get<expr::Not *>(expr->data);
    Type *inner = try$(check_expr(not_->expr, scope));
    not_->expr->expr_type = inner;
    if (!type_eq(inner, new Type{TYPE_BOOL})) {
      return std::unexpected(
          Error("Logical negation requires a boolean operand", expr->start,
                expr->end)
              .with_hint(
                  std::format("Expected `bool`, found `{}`", inner->to_string())));
    }
    type = new Type{TYPE_BOOL, expr->start, expr->end};
    break;
  }

  case EXPR_REF: {
    expr::Ref *ref = std::get<expr::Ref *>(expr->data);
    Type *inner = try$(check_expr(ref->expr, scope));
    ref->expr->expr_type = inner;
    type = new Type{
        .type = TYPE_PTR,
        .start = expr->start,
        .end = expr->end,
        .data = new type::Ptr{inner},
    };
    break;
  }

  case EXPR_DEREF: {
    expr::Deref *deref = std::get<expr::Deref *>(expr->data);
    Type *inner = try$(check_expr(deref->expr, scope));
    deref->expr->expr_type = inner;
    if (inner->type != TYPE_PTR) {
      return std::unexpected(Error("Cannot dereference non-pointer expression",
                                   expr->start, expr->end));
    }

    type = std::get<type::Ptr *>(inner->data)->inner;
    break;
  }

  case EXPR_CALL: {
    expr::Call *call = std::get<expr::Call *>(expr->data);

    std::optional<CheckedProc> proc;
    std::string qualified_name;
    if (call->module.has_value()) {
      std::string_view module = call->module->id_value;
      qualified_name =
          std::format("{}:{}", module, call->name.id_value);
      proc = find_qualified_proc(module, call->name.id_value);
      if (!proc.has_value()) {
        return std::unexpected(
            Error(std::format("Use undeclared procedure `{}`", qualified_name),
                  expr->start, expr->end));
      }
    } else {
      qualified_name = std::string(call->name.id_value);
      proc = find_local_proc(call->name.id_value);
      if (!proc.has_value()) {
        auto imported = try$(find_imported_proc(call->name.id_value, expr->start,
                                               expr->end));
        proc = imported.first;
        call->resolved_module = imported.second;
      }
    }

    if (call->arguments.size() < proc->params.size()) {
      return std::unexpected(
          Error(std::format("Too few arguments for procedure `{}`",
                            qualified_name),
                expr->start, expr->end)
              .with_hint(
                  std::format("`{}` expected {} parameters but {} was provided",
                              qualified_name, proc->params.size(),
                              call->arguments.size())));
    } else if (call->arguments.size() > proc->params.size()) {
      return std::unexpected(
          Error(std::format("Too many arguments for procedure `{}`",
                            qualified_name),
                expr->start, expr->end)
              .with_hint(
                  std::format("`{}` expected {} parameters but {} was provided",
                              qualified_name, proc->params.size(),
                              call->arguments.size())));
    }

    for (size_t i = 0; i < call->arguments.size(); i++) {
      Expr *argument = call->arguments[i];
      Type *arg_type = try$(check_expr(argument, scope));
      Type *param_type = try$(check_type(proc->params[i].type, scope));
      if (!type_eq(param_type, arg_type)) {
        auto error =
            Error("Parameter type mismatch", argument->start, argument->end)
                .with_hint(std::format("Expected `{}`, found `{}`",
                                       param_type->to_string(),
                                       arg_type->to_string()));
        auto fix = suggest_fix(param_type, arg_type, argument);
        if (fix.has_value())
          error.add_help(ErrorHelp(*fix, argument->start, argument->end));
        return std::unexpected(error);
      }

      argument->expr_type = arg_type;
    }

    type = try$(check_type(proc->ret_type, scope));
    break;
  }

  case EXPR_COMPTIME_CALL:
    std::unreachable();
  }

  try$(fold_expr(expr, scope, nullptr));
  expr->expr_type = type;
  return type;
}

ErrorOr<Type *> Checker::check_type(Type *type, Scope *scope) {
  switch (type->type) {
  case TYPE_VOID: return type;
  case TYPE_BOOL: return type;
  case TYPE_INT:  return type;
  case TYPE_PTR:  {
    auto ptr = std::get<type::Ptr *>(type->data);
    Type *inner = try$(check_type(ptr->inner, scope));
    return new Type{
        .type = TYPE_PTR,
        .start = type->start,
        .end = type->end,
        .data = new type::Ptr{inner},
    };
  }
  }
  std::unreachable();
}

std::optional<ComptimeProcInfo>
Checker::find_comptime_proc(std::string_view name) {
  auto it = _comptime_procs.find(std::string(name));
  if (it == _comptime_procs.end())
    return std::nullopt;
  return it->second;
}

ErrorOr<void> Checker::fold_expr(Expr *expr, Scope *scope, ComptimeEnv *env) {
  switch (expr->type) {
  case EXPR_BINARY: {
    auto binary = std::get<expr::Binary *>(expr->data);
    try$(fold_expr(binary->lhs, scope, env));
    try$(fold_expr(binary->rhs, scope, env));
    break;
  }

  case EXPR_GROUP: {
    auto group = std::get<expr::Group *>(expr->data);
    try$(fold_expr(group->expr, scope, env));
    break;
  }

  case EXPR_NOT: {
    auto not_ = std::get<expr::Not *>(expr->data);
    try$(fold_expr(not_->expr, scope, env));
    break;
  }

  default:
    break;
  }

  auto folded = evaluate_constant(expr, scope, env);
  if (folded.has_value()) {
    expr->type = folded.value()->type;
    expr->data = folded.value()->data;
    if (folded.value()->expr_type != nullptr)
      expr->expr_type = folded.value()->expr_type;
  }

  return {};
}

ErrorOr<Expr *> Checker::run_comptime_proc(decl::Proc *proc,
                                           const std::vector<Expr *> &args,
                                           Scope *scope, size_t start,
                                           size_t end) {
  auto info = find_comptime_proc(proc->name.id_value);
  if (!info.has_value()) {
    return std::unexpected(
        Error(std::format("Use undeclared comptime procedure `{}`",
                          proc->name.id_value),
              start, end));
  }

  if (args.size() != info->params.size()) {
    return std::unexpected(
        Error(std::format("Procedure `{}` expected {} arguments but {} were "
                          "provided",
                          proc->name.id_value, info->params.size(),
                          args.size()),
              start, end));
  }

  ComptimeEnv env{};
  for (size_t i = 0; i < args.size(); ++i) {
    Expr *arg = args[i];
    Type *param_type = info->params[i].type;
    Type *arg_type = try$(check_expr(arg, scope));
    if (!type_eq(param_type, arg_type)) {
      return std::unexpected(
          Error("Comptime argument type mismatch", arg->start, arg->end)
              .with_hint(std::format("Expected `{}`, found `{}`",
                                     param_type->to_string(),
                                     arg_type->to_string())));
    }

    Expr *value = try$(evaluate_constant(arg, scope, nullptr));
    env.vars[std::string(info->params[i].name)] = {param_type, value};
  }

  auto result = try$(execute_comptime_stmts(info->proc->body, env, scope,
                                           info->ret_type, start, end));
  if (!result.has_value()) {
    return std::unexpected(
        Error("Comptime procedure did not return a value", start, end));
  }
  return result.value();
}

ErrorOr<std::optional<Expr *>>
Checker::execute_comptime_stmts(const std::vector<Stmt *> &stmts,
                                ComptimeEnv &env, Scope *scope, Type *ret_type,
                                size_t start, size_t end) {
  for (Stmt *stmt: stmts) {
    switch (stmt->type) {
    case STMT_VAR: {
      auto var = std::get<stmt::Var *>(stmt->data);
      Type *type = try$(check_type(var->type, scope));
      if (type->type != TYPE_INT && type->type != TYPE_BOOL) {
        return std::unexpected(
            Error("Comptime variables must have type `int` or `bool`",
                  stmt->start, stmt->end));
      }
      Expr *value = try$(evaluate_constant(var->value, scope, &env));
      Type *value_type = try$(check_expr(value, scope));
      if (!type_eq(type, value_type)) {
        return std::unexpected(
            Error("Comptime variable type mismatch", var->value->start,
                  var->value->end));
      }
      env.vars[std::string(var->name.id_value)] = {type, value};
      break;
    }

    case STMT_ASSIGN: {
      auto assign = std::get<stmt::Assign *>(stmt->data);
      std::string name(assign->name.id_value);
      auto found = env.vars.find(name);
      if (found == env.vars.end()) {
        return std::unexpected(
            Error(std::format("Cannot assign to undeclared comptime variable "
                              "`{}`",
                              name),
                  assign->name.start, assign->name.end));
      }
      Expr *value = try$(evaluate_constant(assign->value, scope, &env));
      Type *value_type = try$(check_expr(value, scope));
      if (!type_eq(found->second.type, value_type)) {
        return std::unexpected(
            Error("Comptime assignment type mismatch", assign->value->start,
                  assign->value->end));
      }
      found->second.value = value;
      break;
    }

    case STMT_RETURN: {
      auto ret = std::get<stmt::Return *>(stmt->data);
      if (!ret->value.has_value()) {
        return std::unexpected(
            Error("Comptime procedure must return a value", stmt->start,
                  stmt->end));
      }
      Expr *value = try$(evaluate_constant(ret->value.value(), scope, &env));
      Type *value_type = try$(check_expr(value, scope));
      if (!type_eq(ret_type, value_type)) {
        return std::unexpected(
            Error("Comptime return type mismatch", ret->value.value()->start,
                  ret->value.value()->end)
                .with_hint(std::format("Expected `{}`, found `{}`",
                                       ret_type->to_string(),
                                       value_type->to_string())));
      }
      return std::optional<Expr *>{value};
    }

    case STMT_IF: {
      auto if_ = std::get<stmt::If *>(stmt->data);
      Expr *cond = try$(evaluate_constant(if_->cond, scope, &env));
      Type *cond_type = try$(check_expr(cond, scope));
      if (!type_eq(cond_type, new Type{TYPE_BOOL})) {
        return std::unexpected(
            Error("Condition has to be a boolean", if_->cond->start,
                  if_->cond->end));
      }
      bool take_then = std::get<expr::Bool *>(cond->data)->value;
      const std::vector<Stmt *> &branch =
          take_then ? if_->then_block : if_->else_block;
      std::optional<Expr *> result = try$(execute_comptime_stmts(
          branch, env, scope, ret_type, start, end));
      if (result.has_value())
        return result;
      break;
    }

    case STMT_WHILE: {
      auto while_ = std::get<stmt::While *>(stmt->data);
      bool exited = false;
      for (size_t guard = 0; guard < 100000; ++guard) {
        Expr *cond = try$(evaluate_constant(while_->cond, scope, &env));
        Type *cond_type = try$(check_expr(cond, scope));
        if (!type_eq(cond_type, new Type{TYPE_BOOL})) {
          return std::unexpected(
              Error("Condition has to be a boolean", while_->cond->start,
                    while_->cond->end));
        }
        if (!std::get<expr::Bool *>(cond->data)->value) {
          exited = true;
          break;
        }

        std::optional<Expr *> result = try$(execute_comptime_stmts(
            while_->body, env, scope, ret_type, start, end));
        if (result.has_value())
          return result;
      }
      if (!exited) {
        return std::unexpected(
            Error("Comptime loop iteration limit exceeded", stmt->start,
                  stmt->end));
      }
      break;
    }

    case STMT_BLOCK: {
      auto block = std::get<stmt::Block *>(stmt->data);
      std::optional<Expr *> result = try$(execute_comptime_stmts(
          block->stmts, env, scope, ret_type, start, end));
      if (result.has_value())
        return result;
      break;
    }

    case STMT_FOR:
    case STMT_WHEN:
    case STMT_BREAK:
    case STMT_CONTINUE:
      return std::unexpected(
          Error("Statement is not supported in comptime code", stmt->start,
                stmt->end));
    }
  }

  return std::nullopt;
}

ErrorOr<void> Checker::check_comptime_proc(decl::Proc *proc, Scope *scope) {
  std::string name(proc->name.id_value);
  if (find_comptime_proc(name).has_value() || find_local_proc(name).has_value()) {
    return std::unexpected(
        Error(std::format("Redeclaration of procedure `{}`", name),
              proc->name.start, proc->name.end));
  }

  if (proc->linkage == LINK_EXTERN) {
    return std::unexpected(
        Error("`comptime` procedures cannot be `extern`", proc->name.start,
              proc->name.end));
  }

  std::set<std::string_view> param_names{};
  std::vector<CheckedParam> params{};
  for (const Param &param: proc->params) {
    std::string_view param_name = param.name.id_value;
    if (param_names.contains(param_name)) {
      return std::unexpected(
          Error(std::format("Parameter with name `{}` already exists",
                            param_name),
                param.name.start, param.name.end));
    }
    Type *type = try$(check_type(param.type, scope));
    if (type->type != TYPE_INT && type->type != TYPE_BOOL) {
      return std::unexpected(
          Error("Comptime procedure parameters must have type `int` or `bool`",
                param.type->start, param.type->end));
    }
    param_names.insert(param_name);
    params.push_back({param_name, type});
  }

  if (!proc->ret_type.has_value()) {
    return std::unexpected(
        Error("Comptime procedure must declare a return type", proc->name.start,
              proc->name.end));
  }

  Type *ret_type = try$(check_type(proc->ret_type.value(), scope));
  if (ret_type->type == TYPE_VOID) {
    return std::unexpected(
        Error("Comptime procedure must return `int` or `bool`", proc->name.start,
              proc->name.end));
  }
  if (ret_type->type != TYPE_INT && ret_type->type != TYPE_BOOL) {
    return std::unexpected(
        Error("Comptime procedure must return `int` or `bool`", proc->name.start,
              proc->name.end));
  }

  ComptimeEnv env{};
  for (const auto &param: params) {
    Expr *dummy = nullptr;
    if (param.type->type == TYPE_INT)
      dummy = new Expr{EXPR_INT, 0, 0, new expr::Int{0}};
    else
      dummy = new Expr{EXPR_BOOL, 0, 0, new expr::Bool{false}};
    env.vars[std::string(param.name)] = {param.type, dummy};
  }

  std::optional<Expr *> validated = try$(execute_comptime_stmts(
      proc->body, env, scope, ret_type, proc->name.start, proc->name.end));
  if (!validated.has_value()) {
    return std::unexpected(
        Error("Comptime procedure must return a value on all paths",
              proc->name.start, proc->name.end));
  }

  _comptime_procs[name] = ComptimeProcInfo{
      .proc = proc,
      .params = params,
      .ret_type = ret_type,
  };

  return {};
}

ErrorOr<Expr *> Checker::evaluate_constant(Expr *expr, Scope *scope,
                                           ComptimeEnv *env) {
  switch (expr->type) {
  case EXPR_INT:
  case EXPR_BOOL: return expr;

  case EXPR_COMPTIME_CALL: {
    auto call = std::get<expr::ComptimeCall *>(expr->data);
    auto info = find_comptime_proc(call->name.id_value);
    if (!info.has_value()) {
      return std::unexpected(
          Error(std::format("Use undeclared comptime procedure `{}`",
                            call->name.id_value),
                expr->start, expr->end));
    }

    std::vector<Expr *> args{};
    for (Expr *arg: call->arguments)
      args.push_back(try$(evaluate_constant(arg, scope, env)));

    return run_comptime_proc(info->proc, args, scope, expr->start, expr->end);
  }

  case EXPR_VAR:  {
    auto var = std::get<expr::Var *>(expr->data);
    if (var->module.has_value()) {
      auto constant =
          find_qualified_const(var->module->id_value, var->var.id_value);
      if (!constant.has_value()) {
        return std::unexpected(
            Error(std::format("Use undeclared constant `{}:{}`",
                              var->module->id_value, var->var.id_value),
                  expr->start, expr->end));
      }
      return constant->expr;
    }

    for (const auto &local: _consts) {
      if (local.name == var->var.id_value)
        return local.expr;
    }

    if (env != nullptr) {
      auto found = env->vars.find(std::string(var->var.id_value));
      if (found != env->vars.end())
        return found->second.value;
    }

    if (!_imports.empty()) {
      CheckedConst imported =
          try$(find_imported_const(var->var.id_value, expr->start, expr->end));
      return imported.expr;
    }

    return std::unexpected(
        Error(std::format("Use undeclared constant `{}`", var->var.id_value),
              expr->start, expr->end));
  }

  case EXPR_BINARY: {
    auto binary = std::get<expr::Binary *>(expr->data);

    Expr *lhs = try$(evaluate_constant(binary->lhs, scope, env));
    Expr *rhs = try$(evaluate_constant(binary->rhs, scope, env));

    switch (binary->op) {
    case expr::Binary::BINOP_PLUS: {
      if (lhs->type != EXPR_INT)
        PANIC("TODO: support other than ints as LHS in +");
      if (rhs->type != EXPR_INT)
        PANIC("TODO: support other than ints as RHS in +");

      int64_t lhs_value = std::get<expr::Int *>(lhs->data)->value;
      int64_t rhs_value = std::get<expr::Int *>(rhs->data)->value;
      return new Expr{EXPR_INT, expr->start, expr->end,
                      new expr::Int{lhs_value + rhs_value}};
    }

    case expr::Binary::BINOP_MINUS: {
      if (lhs->type != EXPR_INT || rhs->type != EXPR_INT)
        PANIC("TODO: support non-int constant arithmetic");
      int64_t l = std::get<expr::Int *>(lhs->data)->value;
      int64_t r = std::get<expr::Int *>(rhs->data)->value;
      return new Expr{EXPR_INT, expr->start, expr->end, new expr::Int{l - r}};
    }

    case expr::Binary::BINOP_STAR: {
      if (lhs->type != EXPR_INT || rhs->type != EXPR_INT)
        PANIC("TODO: support non-int constant arithmetic");
      int64_t l = std::get<expr::Int *>(lhs->data)->value;
      int64_t r = std::get<expr::Int *>(rhs->data)->value;
      return new Expr{EXPR_INT, expr->start, expr->end, new expr::Int{l * r}};
    }

    case expr::Binary::BINOP_SLASH: {
      if (lhs->type != EXPR_INT || rhs->type != EXPR_INT)
        PANIC("TODO: support non-int constant arithmetic");
      int64_t l = std::get<expr::Int *>(lhs->data)->value;
      int64_t r = std::get<expr::Int *>(rhs->data)->value;
      return new Expr{EXPR_INT, expr->start, expr->end, new expr::Int{l / r}};
    }

    case expr::Binary::BINOP_EQ: {
      if (lhs->type == EXPR_BOOL && rhs->type == EXPR_BOOL) {
        bool l = std::get<expr::Bool *>(lhs->data)->value;
        bool r = std::get<expr::Bool *>(rhs->data)->value;
        return new Expr{EXPR_BOOL, expr->start, expr->end, new expr::Bool{l == r}};
      }
      if (lhs->type != EXPR_INT)
        PANIC("TODO: support other than ints as LHS in ==");
      if (rhs->type != EXPR_INT)
        PANIC("TODO: support other than ints as RHS in ==");

      int64_t lhs_value = std::get<expr::Int *>(lhs->data)->value;
      int64_t rhs_value = std::get<expr::Int *>(rhs->data)->value;
      return new Expr{EXPR_BOOL, expr->start, expr->end,
                      new expr::Bool{lhs_value == rhs_value}};
    }

    case expr::Binary::BINOP_NEQ: {
      if (lhs->type == EXPR_BOOL && rhs->type == EXPR_BOOL) {
        bool l = std::get<expr::Bool *>(lhs->data)->value;
        bool r = std::get<expr::Bool *>(rhs->data)->value;
        return new Expr{EXPR_BOOL, expr->start, expr->end, new expr::Bool{l != r}};
      }
      if (lhs->type != EXPR_INT || rhs->type != EXPR_INT)
        PANIC("TODO: support non-int constant comparisons");
      int64_t l = std::get<expr::Int *>(lhs->data)->value;
      int64_t r = std::get<expr::Int *>(rhs->data)->value;
      return new Expr{EXPR_BOOL, expr->start, expr->end, new expr::Bool{l != r}};
    }

    case expr::Binary::BINOP_LT: {
      if (lhs->type != EXPR_INT || rhs->type != EXPR_INT)
        PANIC("TODO: support non-int constant comparisons");
      int64_t l = std::get<expr::Int *>(lhs->data)->value;
      int64_t r = std::get<expr::Int *>(rhs->data)->value;
      return new Expr{EXPR_BOOL, expr->start, expr->end, new expr::Bool{l < r}};
    }

    case expr::Binary::BINOP_LTE: {
      if (lhs->type != EXPR_INT || rhs->type != EXPR_INT)
        PANIC("TODO: support non-int constant comparisons");
      int64_t l = std::get<expr::Int *>(lhs->data)->value;
      int64_t r = std::get<expr::Int *>(rhs->data)->value;
      return new Expr{EXPR_BOOL, expr->start, expr->end, new expr::Bool{l <= r}};
    }

    case expr::Binary::BINOP_GT: {
      if (lhs->type != EXPR_INT || rhs->type != EXPR_INT)
        PANIC("TODO: support non-int constant comparisons");
      int64_t l = std::get<expr::Int *>(lhs->data)->value;
      int64_t r = std::get<expr::Int *>(rhs->data)->value;
      return new Expr{EXPR_BOOL, expr->start, expr->end, new expr::Bool{l > r}};
    }

    case expr::Binary::BINOP_GTE: {
      if (lhs->type != EXPR_INT || rhs->type != EXPR_INT)
        PANIC("TODO: support non-int constant comparisons");
      int64_t l = std::get<expr::Int *>(lhs->data)->value;
      int64_t r = std::get<expr::Int *>(rhs->data)->value;
      return new Expr{EXPR_BOOL, expr->start, expr->end, new expr::Bool{l >= r}};
    }

    case expr::Binary::BINOP_AND: {
      if (lhs->type != EXPR_BOOL || rhs->type != EXPR_BOOL)
        PANIC("TODO: support non-bool constant logical ops");
      bool l = std::get<expr::Bool *>(lhs->data)->value;
      bool r = std::get<expr::Bool *>(rhs->data)->value;
      return new Expr{EXPR_BOOL, expr->start, expr->end,
                      new expr::Bool{l && r}};
    }

    case expr::Binary::BINOP_OR: {
      if (lhs->type != EXPR_BOOL || rhs->type != EXPR_BOOL)
        PANIC("TODO: support non-bool constant logical ops");
      bool l = std::get<expr::Bool *>(lhs->data)->value;
      bool r = std::get<expr::Bool *>(rhs->data)->value;
      return new Expr{EXPR_BOOL, expr->start, expr->end,
                      new expr::Bool{l || r}};
    }
    }
    std::unreachable();
  }

  case EXPR_GROUP: {
    auto group = std::get<expr::Group *>(expr->data);
    return evaluate_constant(group->expr, scope, env);
  }

  case EXPR_NOT: {
    auto not_ = std::get<expr::Not *>(expr->data);
    Expr *inner = try$(evaluate_constant(not_->expr, scope, env));
    if (inner->type != EXPR_BOOL)
      PANIC("TODO: support non-bool constant negation");
    bool value = std::get<expr::Bool *>(inner->data)->value;
    return new Expr{EXPR_BOOL, expr->start, expr->end, new expr::Bool{!value}};
  }

  default:
    return std::unexpected(
        Error("Cannot evaluate non-constant expression at compile time",
              expr->start, expr->end)
            .with_hint("Not a constant expression"));
  }
}

bool Checker::type_eq(Type *a, Type *b) {
  switch (a->type) {
  case TYPE_VOID: return b->type == TYPE_VOID;
  case TYPE_BOOL: return b->type == TYPE_BOOL;
  case TYPE_INT:  return b->type == TYPE_INT;
  case TYPE_PTR:  {
    Type *a_inner = std::get<type::Ptr *>(a->data)->inner;
    if (b->type != TYPE_PTR)
      return false;

    Type *b_inner = std::get<type::Ptr *>(b->data)->inner;
    return type_eq(a_inner, b_inner);
  }
  }
  std::unreachable();
}

std::optional<CheckedProc> Checker::find_local_proc(std::string_view name) {
  for (const auto &proc: _procs) {
    if (proc.name == name)
      return proc;
  }

  return std::nullopt;
}

std::optional<CheckedProc>
Checker::find_qualified_proc(std::string_view module, std::string_view name) {
  if (_registry == nullptr)
    return std::nullopt;
  return _registry->find_proc(module, name);
}

std::optional<CheckedConst> Checker::find_const(std::string_view name) {
  for (const auto &const_: _consts) {
    if (const_.name == name)
      return const_;
  }

  return std::nullopt;
}

std::optional<CheckedConst>
Checker::find_qualified_const(std::string_view module, std::string_view name) {
  if (_registry == nullptr)
    return std::nullopt;
  return _registry->find_const(module, name);
}

ErrorOr<std::pair<CheckedProc, std::string>>
Checker::find_imported_proc(std::string_view name, size_t start, size_t end) {
  if (_registry == nullptr || _imports.empty()) {
    return std::unexpected(
        Error(std::format("Use undeclared procedure `{}`", name), start, end));
  }

  std::optional<CheckedProc> found;
  std::optional<std::string> found_module;
  for (const auto &import_module: _imports) {
    auto proc = find_qualified_proc(import_module, name);
    if (!proc.has_value())
      continue;
    if (found.has_value()) {
      return std::unexpected(
          Error(std::format("Ambiguous import for procedure `{}`", name),
                start, end)
              .with_hint("Multiple imported modules export a procedure with "
                         "this name; use a qualified call"));
    }
    found = proc;
    found_module = import_module;
  }

  if (!found.has_value()) {
    return std::unexpected(
        Error(std::format("Use undeclared procedure `{}`", name), start, end)
            .with_hint("Import the module that defines this procedure, or "
                       "call it with `module:proc`"));
  }

  return std::make_pair(*found, *found_module);
}

ErrorOr<CheckedConst> Checker::find_imported_const(std::string_view name,
                                                   size_t start, size_t end) {
  if (_registry == nullptr || _imports.empty()) {
    return std::unexpected(
        Error(std::format("Use undeclared constant `{}`", name), start, end));
  }

  std::optional<CheckedConst> found;
  std::optional<std::string> found_module;
  for (const auto &import_module: _imports) {
    auto constant = find_qualified_const(import_module, name);
    if (!constant.has_value())
      continue;
    if (found.has_value()) {
      return std::unexpected(
          Error(std::format("Ambiguous import for constant `{}`", name), start,
                end)
              .with_hint("Multiple imported modules export a constant with "
                         "this name; use `module:const`"));
    }
    found = constant;
    found_module = import_module;
  }

  if (!found.has_value()) {
    return std::unexpected(
        Error(std::format("Use undeclared constant `{}`", name), start, end));
  }

  return *found;
}
