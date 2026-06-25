#include <checker/checker.hpp>

#include <format>

#include <arena.hpp>
#include <parser/comptime_parse.hpp>

Type *Checker::comptime_string_type() {
  if (_comptime_string_type == nullptr)
    _comptime_string_type =
        new Type{.type = TYPE_STRUCT,
                 .start = 0,
                 .end = 0,
                 .data = new type::Struct{type::Struct{.name = "ComptimeString"}}};
  return _comptime_string_type;
}

Type *Checker::comptime_stmt_type() {
  if (_comptime_stmt_type == nullptr)
    _comptime_stmt_type =
        new Type{.type = TYPE_STRUCT,
                 .start = 0,
                 .end = 0,
                 .data = new type::Struct{type::Struct{.name = "ComptimeStmt"}}};
  return _comptime_stmt_type;
}

Type *Checker::comptime_decl_type() {
  if (_comptime_decl_type == nullptr)
    _comptime_decl_type =
        new Type{.type = TYPE_STRUCT,
                 .start = 0,
                 .end = 0,
                 .data = new type::Struct{type::Struct{.name = "ComptimeDecl"}}};
  return _comptime_decl_type;
}

bool Checker::is_comptime_string_type(Type *type) const {
  return type != nullptr && type->type == TYPE_STRUCT &&
         std::get<type::Struct *>(type->data)->name == "ComptimeString";
}

bool Checker::is_comptime_stmt_type(Type *type) const {
  return type != nullptr && type->type == TYPE_STRUCT &&
         std::get<type::Struct *>(type->data)->name == "ComptimeStmt";
}

bool Checker::is_comptime_decl_type(Type *type) const {
  return type != nullptr && type->type == TYPE_STRUCT &&
         std::get<type::Struct *>(type->data)->name == "ComptimeDecl";
}

static std::string intrinsic_key(std::string_view module, std::string_view name) {
  return std::format("{}:{}", module, name);
}

bool Checker::is_intrinsic_comptime_proc(std::string_view module,
                                         std::string_view name) const {
  return _intrinsic_comptime_procs.contains(intrinsic_key(module, name));
}

ErrorOr<void> Checker::register_intrinsic_comptime_procs() {
  comptime_string_type();
  comptime_stmt_type();
  comptime_decl_type();

  auto mark_intrinsic = [&](std::string_view module, std::string_view name,
                            std::vector<CheckedParam> params, Type *ret) {
    _intrinsic_comptime_procs[intrinsic_key(module, name)] =
        ComptimeProcInfo{.proc = nullptr,
                         .params = std::move(params),
                         .ret_type = ret,
                         .intrinsic = true};
  };

  mark_intrinsic("compiler", "parse_stmt",
                 {{CheckedParam{"source", comptime_string_type()}}},
                 comptime_stmt_type());
  mark_intrinsic("compiler", "inject_stmt",
                 {{CheckedParam{"stmt", comptime_stmt_type()}}},
                 new Type{TYPE_VOID, 0, 0});
  mark_intrinsic("compiler", "parse_decl",
                 {{CheckedParam{"source", comptime_string_type()}}},
                 comptime_decl_type());
  mark_intrinsic("compiler", "inject_decl",
                 {{CheckedParam{"decl", comptime_decl_type()}}},
                 new Type{TYPE_VOID, 0, 0});
  mark_intrinsic("compiler", "error",
                 {{CheckedParam{"message", comptime_string_type()}}},
                 new Type{TYPE_VOID, 0, 0});
  mark_intrinsic("string", "to_str", {{CheckedParam{"value", new Type{TYPE_INT, 0, 0}}}},
                 comptime_string_type());

  return {};
}

ErrorOr<ComptimeValue> Checker::run_intrinsic_comptime_proc(
    std::string_view module, std::string_view name,
    const std::vector<ComptimeValue> &args, ComptimeExpansion *expansion,
    size_t start, size_t end) {
  if (!is_intrinsic_comptime_proc(module, name)) {
    return std::unexpected(
        Error(std::format("Unknown intrinsic comptime procedure `{}:{}`",
                          module, name),
              start, end));
  }

  if (module == "compiler" && name == "error") {
    if (args.size() != 1 || args[0].kind != ComptimeValueKind::String) {
      return std::unexpected(
          Error("compiler:error expects a string message", start, end));
    }
    return std::unexpected(Error(args[0].string_value, start, end));
  }

  if (_arena == nullptr) {
    return std::unexpected(
        Error("Compiler intrinsics require an AST arena", start, end));
  }

  if (module == "compiler" && name == "parse_stmt") {
    if (args.size() != 1 || args[0].kind != ComptimeValueKind::String) {
      return std::unexpected(
          Error("compiler:parse_stmt expects a string source", start, end));
    }
    Stmt *stmt = try$(parse_single_stmt(*_arena, args[0].string_value));
    return ComptimeValue{.kind = ComptimeValueKind::Stmt, .stmt = stmt};
  }

  if (module == "compiler" && name == "parse_decl") {
    if (args.size() != 1 || args[0].kind != ComptimeValueKind::String) {
      return std::unexpected(
          Error("compiler:parse_decl expects a string source", start, end));
    }
    Decl *decl = try$(parse_single_decl(*_arena, args[0].string_value));
    return ComptimeValue{.kind = ComptimeValueKind::Decl, .decl = decl};
  }

  if (module == "compiler" && name == "inject_stmt") {
    if (expansion == nullptr) {
      return std::unexpected(
          Error("compiler:inject_stmt requires a comptime expansion context",
                start, end));
    }
    if (expansion->target != ComptimeExpansionTarget::StmtList) {
      return std::unexpected(
          Error("compiler:inject_stmt is only valid inside a procedure body",
                start, end));
    }
    if (args.size() != 1 || args[0].kind != ComptimeValueKind::Stmt ||
        args[0].stmt == nullptr) {
      return std::unexpected(
          Error("compiler:inject_stmt expects a ComptimeStmt", start, end));
    }
    expansion->injected_stmts.push_back(args[0].stmt);
    return ComptimeValue{.kind = ComptimeValueKind::Int, .int_value = 0};
  }

  if (module == "compiler" && name == "inject_decl") {
    if (expansion == nullptr) {
      return std::unexpected(
          Error("compiler:inject_decl requires a comptime expansion context",
                start, end));
    }
    if (expansion->target != ComptimeExpansionTarget::Module) {
      return std::unexpected(
          Error("compiler:inject_decl is only valid at module scope", start,
                end));
    }
    if (args.size() != 1 || args[0].kind != ComptimeValueKind::Decl ||
        args[0].decl == nullptr) {
      return std::unexpected(
          Error("compiler:inject_decl expects a ComptimeDecl", start, end));
    }
    expansion->injected_decls.push_back(args[0].decl);
    return ComptimeValue{.kind = ComptimeValueKind::Int, .int_value = 0};
  }

  if (module == "string" && name == "to_str") {
    if (args.size() != 1 || args[0].kind != ComptimeValueKind::Int) {
      return std::unexpected(
          Error("string:to_str expects an int", start, end));
    }
    return ComptimeValue{.kind = ComptimeValueKind::String,
                         .string_value = std::to_string(args[0].int_value)};
  }

  return std::unexpected(
      Error(std::format("Intrinsic comptime procedure `{}:{}` is not implemented",
                        module, name),
            start, end));
}

ErrorOr<ComptimeValue> Checker::evaluate_comptime_value(Expr *expr, Scope *scope,
                                                        ComptimeEnv &env,
                                                        ComptimeExpansion *expansion) {
  switch (expr->type) {
  case EXPR_INT:
    return ComptimeValue{.kind = ComptimeValueKind::Int,
                         .int_value = std::get<expr::Int *>(expr->data)->value};
  case EXPR_BOOL:
    return ComptimeValue{.kind = ComptimeValueKind::Bool,
                         .bool_value = std::get<expr::Bool *>(expr->data)->value};
  case EXPR_STRING: {
    auto *str = std::get<expr::String *>(expr->data);
    return ComptimeValue{.kind = ComptimeValueKind::String,
                         .string_value = unescape_string(str->value.string_value)};
  }
  case EXPR_VAR: {
    auto *var = std::get<expr::Var *>(expr->data);
    if (var->module.has_value()) {
      auto constant =
          find_qualified_const(var->module->id_value, var->var.id_value);
      if (!constant.has_value()) {
        return std::unexpected(
            Error(std::format("Use of undeclared constant `{}:{}`",
                              var->module->id_value, var->var.id_value),
                  expr->start, expr->end));
      }
      return evaluate_comptime_value(constant->expr, scope, env, expansion);
    }

    auto found = env.vars.find(std::string(var->var.id_value));
    if (found != env.vars.end())
      return found->second.value;

    for (const auto &local: _consts) {
      if (local.name == var->var.id_value)
        return evaluate_comptime_value(local.expr, scope, env, expansion);
    }

    return std::unexpected(
        Error(std::format("Use of undeclared comptime variable `{}`",
                          var->var.id_value),
              expr->start, expr->end));
  }
  case EXPR_COMPTIME_CALL: {
    auto *call = std::get<expr::ComptimeCall *>(expr->data);
    auto info = find_comptime_proc(call->name.id_value);
    if (!info.has_value()) {
      return std::unexpected(
          Error(std::format("Use of undeclared comptime procedure `{}`",
                            call->name.id_value),
                expr->start, expr->end));
    }
    std::vector<Expr *> args{};
    for (Expr *arg: call->arguments)
      args.push_back(arg);
    Expr *result = try$(run_comptime_proc(info->proc, args, scope, expr->start,
                                           expr->end));
    if (result->type == EXPR_INT)
      return ComptimeValue{.kind = ComptimeValueKind::Int,
                           .int_value = std::get<expr::Int *>(result->data)->value};
    if (result->type == EXPR_BOOL)
      return ComptimeValue{.kind = ComptimeValueKind::Bool,
                           .bool_value = std::get<expr::Bool *>(result->data)->value};
    return std::unexpected(
        Error("Comptime procedure returned unsupported value type", expr->start,
              expr->end));
  }
  case EXPR_CALL: {
    auto *call = std::get<expr::Call *>(expr->data);
    if (!call->name.has_value()) {
      return std::unexpected(
          Error("Comptime calls require a named procedure", expr->start, expr->end));
    }
    std::string_view module_name = _module_name;
    if (call->module.has_value())
      module_name = call->module->id_value;
    else if (call->resolved_module.has_value())
      module_name = *call->resolved_module;

    std::vector<ComptimeValue> args{};
    for (Expr *arg: call->arguments) {
      ComptimeValue value =
          try$(evaluate_comptime_value(arg, scope, env, expansion));
      args.push_back(std::move(value));
    }

    if (is_intrinsic_comptime_proc(module_name, call->name->id_value)) {
      return run_intrinsic_comptime_proc(module_name, call->name->id_value, args,
                                         expansion, expr->start, expr->end);
    }

    return std::unexpected(
        Error(std::format("Procedure `{}:{}` is not available at comptime",
                          module_name, call->name->id_value),
              expr->start, expr->end));
  }
  case EXPR_BINARY: {
    auto *binary = std::get<expr::Binary *>(expr->data);
    ComptimeValue lhs =
        try$(evaluate_comptime_value(binary->lhs, scope, env, expansion));
    ComptimeValue rhs =
        try$(evaluate_comptime_value(binary->rhs, scope, env, expansion));

    if (binary->op == expr::Binary::BINOP_PLUS) {
      if (lhs.kind == ComptimeValueKind::String &&
          rhs.kind == ComptimeValueKind::String) {
        return ComptimeValue{.kind = ComptimeValueKind::String,
                             .string_value = lhs.string_value + rhs.string_value};
      }
      if (lhs.kind == ComptimeValueKind::Int && rhs.kind == ComptimeValueKind::Int) {
        return ComptimeValue{.kind = ComptimeValueKind::Int,
                             .int_value = lhs.int_value + rhs.int_value};
      }
    }

    if (lhs.kind == ComptimeValueKind::Int && rhs.kind == ComptimeValueKind::Int) {
      switch (binary->op) {
      case expr::Binary::BINOP_MINUS:
        return ComptimeValue{.kind = ComptimeValueKind::Int,
                           .int_value = lhs.int_value - rhs.int_value};
      case expr::Binary::BINOP_STAR:
        return ComptimeValue{.kind = ComptimeValueKind::Int,
                             .int_value = lhs.int_value * rhs.int_value};
      case expr::Binary::BINOP_SLASH:
        return ComptimeValue{.kind = ComptimeValueKind::Int,
                             .int_value = lhs.int_value / rhs.int_value};
      case expr::Binary::BINOP_EQ:
        return ComptimeValue{.kind = ComptimeValueKind::Bool,
                             .bool_value = lhs.int_value == rhs.int_value};
      case expr::Binary::BINOP_NEQ:
        return ComptimeValue{.kind = ComptimeValueKind::Bool,
                             .bool_value = lhs.int_value != rhs.int_value};
      case expr::Binary::BINOP_LT:
        return ComptimeValue{.kind = ComptimeValueKind::Bool,
                             .bool_value = lhs.int_value < rhs.int_value};
      case expr::Binary::BINOP_LTE:
        return ComptimeValue{.kind = ComptimeValueKind::Bool,
                             .bool_value = lhs.int_value <= rhs.int_value};
      case expr::Binary::BINOP_GT:
        return ComptimeValue{.kind = ComptimeValueKind::Bool,
                             .bool_value = lhs.int_value > rhs.int_value};
      case expr::Binary::BINOP_GTE:
        return ComptimeValue{.kind = ComptimeValueKind::Bool,
                             .bool_value = lhs.int_value >= rhs.int_value};
      default:
        break;
      }
    }

  return std::unexpected(
      Error("Invalid comptime binary operation", expr->start, expr->end));
  }
  case EXPR_GROUP:
    return evaluate_comptime_value(std::get<expr::Group *>(expr->data)->expr,
                                   scope, env, expansion);
  case EXPR_NOT: {
    ComptimeValue inner =
        try$(evaluate_comptime_value(std::get<expr::Not *>(expr->data)->expr,
                                     scope, env, expansion));
    if (inner.kind != ComptimeValueKind::Bool) {
      return std::unexpected(
          Error("Comptime logical not requires a boolean operand", expr->start,
                expr->end));
    }
    return ComptimeValue{.kind = ComptimeValueKind::Bool,
                         .bool_value = !inner.bool_value};
  }
  default:
    return std::unexpected(
        Error("Expression is not supported in comptime code", expr->start,
              expr->end));
  }
}

bool Checker::comptime_value_matches_type(const ComptimeValue &value,
                                          Type *type) const {
  switch (value.kind) {
  case ComptimeValueKind::Int:
    return type->type == TYPE_INT;
  case ComptimeValueKind::Bool:
    return type->type == TYPE_BOOL;
  case ComptimeValueKind::String:
    return is_comptime_string_type(type);
  case ComptimeValueKind::Stmt:
    return is_comptime_stmt_type(type);
  case ComptimeValueKind::Decl:
    return is_comptime_decl_type(type);
  }
  std::unreachable();
}

Type *Checker::type_for_comptime_value(const ComptimeValue &value) {
  switch (value.kind) {
  case ComptimeValueKind::Int:
    return new Type{TYPE_INT, 0, 0};
  case ComptimeValueKind::Bool:
    return new Type{TYPE_BOOL, 0, 0};
  case ComptimeValueKind::String:
    return comptime_string_type();
  case ComptimeValueKind::Stmt:
    return comptime_stmt_type();
  case ComptimeValueKind::Decl:
    return comptime_decl_type();
  }
  std::unreachable();
}

Expr *Checker::comptime_value_to_expr(const ComptimeValue &value, size_t start,
                                      size_t end) const {
  switch (value.kind) {
  case ComptimeValueKind::Int:
    return new Expr{EXPR_INT, start, end,
                    new expr::Int{value.int_value}};
  case ComptimeValueKind::Bool:
    return new Expr{EXPR_BOOL, start, end,
                    new expr::Bool{value.bool_value}};
  default:
    return nullptr;
  }
}

ErrorOr<void> Checker::expand_comptime_block(decl::ComptimeBlock *block,
                                             Scope *scope, size_t start,
                                             size_t end,
                                             ComptimeExpansionTarget target,
                                             std::vector<Decl *> &out_decls,
                                             std::vector<Stmt *> &out_stmts) {
  ComptimeExpansion expansion{.target = target};
  ComptimeEnv env{};
  Type *ret_type = new Type{TYPE_VOID, start, end};
  try$(execute_comptime_stmts(block->stmts, env, scope, ret_type, start, end,
                              &expansion));

  out_decls.insert(out_decls.end(), block->decls.begin(), block->decls.end());
  out_decls.insert(out_decls.end(), expansion.injected_decls.begin(),
                    expansion.injected_decls.end());
  out_stmts.insert(out_stmts.end(), expansion.injected_stmts.begin(),
                   expansion.injected_stmts.end());
  return {};
}

ErrorOr<void> Checker::expand_comptime_stmts(std::vector<Stmt *> &stmts,
                                             Scope *scope) {
  for (size_t i = 0; i < stmts.size();) {
    Stmt *stmt = stmts[i];
    if (stmt->type == STMT_COMPTIME_BLOCK) {
      auto *block = std::get<decl::ComptimeBlock *>(stmt->data);
      std::vector<Decl *> decls{};
      std::vector<Stmt *> injected{};
      try$(expand_comptime_block(block, scope, stmt->start, stmt->end,
                                ComptimeExpansionTarget::StmtList, decls,
                                injected));
      if (!decls.empty()) {
        return std::unexpected(
            Error("Declarations are not allowed in procedure comptime blocks",
                  stmt->start, stmt->end));
      }
      stmts.erase(stmts.begin() + static_cast<std::ptrdiff_t>(i));
      stmts.insert(stmts.begin() + static_cast<std::ptrdiff_t>(i),
                   injected.begin(), injected.end());
      continue;
    }

    if (stmt->type == STMT_BLOCK) {
      auto *block = std::get<stmt::Block *>(stmt->data);
      try$(expand_comptime_stmts(block->stmts, scope));
    } else if (stmt->type == STMT_IF) {
      auto *iff = std::get<stmt::If *>(stmt->data);
      try$(expand_comptime_stmts(iff->then_block, scope));
      try$(expand_comptime_stmts(iff->else_block, scope));
    } else if (stmt->type == STMT_WHILE) {
      auto *loop = std::get<stmt::While *>(stmt->data);
      try$(expand_comptime_stmts(loop->body, scope));
    } else if (stmt->type == STMT_FOR) {
      auto *loop = std::get<stmt::For *>(stmt->data);
      try$(expand_comptime_stmts(loop->body, scope));
    }

    ++i;
  }

  return {};
}
