#include <utility>

#include <checker/checker.hpp>
#include <host.hpp>
#include "parser/ast.hpp"

ErrorOr<void> Checker::check_decls(const std::vector<Decl *> &decls) {
  _consts.push_back(CheckedConst{"OS_LINUX", new Type{TYPE_INT},
                                 new Expr{EXPR_INT, 0, 0, new expr::Int{0}}});
  _consts.push_back(CheckedConst{"OS_MACOS", new Type{TYPE_INT},
                                 new Expr{EXPR_INT, 0, 0, new expr::Int{1}}});
  _consts.push_back(CheckedConst{"TARGET_OS", new Type{TYPE_INT},
                                 new Expr{EXPR_INT, 0, 0, new expr::Int{HOST_OS}}});

  for (Decl *decl: decls)
    try$(check_decl(decl, _global_scope));
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
    if (find_proc(proc->name.id_value).has_value()) {
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

    for (auto *stmt: proc->body)
      try$(check_stmt(stmt, proc_scope));

    _current_proc_id = std::numeric_limits<size_t>::max();

    return {};
  }

  case DECL_WHEN: {
    decl::When *when = std::get<decl::When *>(decl->data);

    Expr *cond = try$(evaluate_constant(when->cond, scope));
    when->cond = cond;

    Type *cond_type = try$(check_expr(cond, scope));
    if (!type_eq(cond_type, new Type{TYPE_BOOL})) {
      return std::unexpected(
          Error("Condition has to be a boolean", cond->start, cond->end)
              .with_hint(std::format("Expected `bool`, found `{}`",
                                     cond_type->to_string())));
    }

    when->cond->expr_type =
        new Type{TYPE_BOOL, cond_type->start, cond_type->end};


    bool value = std::get<expr::Bool *>(cond->data)->value;

    std::vector<Decl *> block = value ? when->true_block : when->false_block;
    if (block.size() == 0)
      return std::unexpected(
          Error("Empty branch in `when` declaration", decl->start, decl->end)
              .with_hint(
                  "Expected at least one branch to contain executable code"));

    if (block.size() > 1)
      return std::unexpected(
          Error("The branches in `when` declarations currently only support "
                "one declaration each",
                decl->start, decl->end));

    try$(check_decl(block.back(), _global_scope));
    *decl = *block.back();

    return {};
  }
  }
}

ErrorOr<void> Checker::check_stmt(Stmt *stmt, Scope *scope) {
  switch (stmt->type) {
  case STMT_BLOCK: {
    stmt::Block *block = std::get<stmt::Block *>(stmt->data);
    for (Stmt *inner: block->stmts)
      try$(check_stmt(inner, scope));
    return {};
  }

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
    for (Stmt *inner: while_->body)
      try$(check_stmt(inner, scope));
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
    for (Stmt *inner: for_->body)
      try$(check_stmt(inner, scope));
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
  }
}

ErrorOr<Type *> Checker::check_expr(Expr *expr, Scope *scope) {
  switch (expr->type) {
  case EXPR_INT:  return new Type{TYPE_INT, expr->start, expr->end};

  case EXPR_BOOL: return new Type{TYPE_BOOL, expr->start, expr->end};

  case EXPR_VAR:  {
    expr::Var *var = std::get<expr::Var *>(expr->data);

    auto const_ = find_const(var->var.id_value);
    if (const_.has_value())
      return const_->type;

    auto found = scope->find_var(var->var.id_value);
    if (found.has_value())
      return found.value();

    return std::unexpected(
        Error(std::format("Use undeclared variable `{}`", var->var.id_value),

              expr->start, expr->end));
  }

  case EXPR_GROUP: {
    expr::Group *group = std::get<expr::Group *>(expr->data);
    Type *inner = try$(check_expr(group->expr, scope));
    group->expr->expr_type = inner;
    return inner;
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
    case expr::Binary::BINOP_SLASH: return lhs_type;

    case expr::Binary::BINOP_EQ:
    case expr::Binary::BINOP_NEQ:
    case expr::Binary::BINOP_LT:
    case expr::Binary::BINOP_LTE:
    case expr::Binary::BINOP_GT:
    case expr::Binary::BINOP_GTE:   return new Type{TYPE_BOOL};
    }
  }

  case EXPR_REF: {
    expr::Ref *ref = std::get<expr::Ref *>(expr->data);
    Type *inner = try$(check_expr(ref->expr, scope));
    ref->expr->expr_type = inner;
    return new Type{
        .type = TYPE_PTR,
        .start = expr->start,
        .end = expr->end,
        .data = new type::Ptr{inner},
    };
  }

  case EXPR_DEREF: {
    expr::Deref *deref = std::get<expr::Deref *>(expr->data);
    Type *inner = try$(check_expr(deref->expr, scope));
    deref->expr->expr_type = inner;
    if (inner->type != TYPE_PTR) {
      return std::unexpected(Error("Cannot dereference non-pointer expression",
                                   expr->start, expr->end));
    }

    return std::get<type::Ptr *>(inner->data)->inner;
  }

  case EXPR_CALL: {
    expr::Call *call = std::get<expr::Call *>(expr->data);

    auto proc = find_proc(call->name.id_value);
    if (!proc.has_value())
      return std::unexpected(Error(
          std::format("Use undeclared procedure `{}`", call->name.id_value),
          expr->start, expr->end));

    if (call->arguments.size() < proc->params.size()) {
      return std::unexpected(
          Error(std::format("Too few arguments for procedure `{}`",
                            call->name.id_value),
                expr->start, expr->end)
              .with_hint(
                  std::format("`{}` expected {} parameters but {} was provided",
                              call->name.id_value, proc->params.size(),
                              call->arguments.size())));
    } else if (call->arguments.size() > proc->params.size()) {
      return std::unexpected(
          Error(std::format("Too many arguments for procedure `{}`",
                            call->name.id_value),
                expr->start, expr->end)
              .with_hint(
                  std::format("`{}` expected {} parameters but {} was provided",
                              call->name.id_value, proc->params.size(),
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

    Type *ret_type = try$(check_type(proc->ret_type, scope));
    expr->expr_type = ret_type;
    return ret_type;
  }
  }
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
}

ErrorOr<Expr *> Checker::evaluate_constant(Expr *expr, Scope *scope) {
  switch (expr->type) {
  case EXPR_INT:
  case EXPR_BOOL: return expr;

  case EXPR_VAR:  {
    auto var = std::get<expr::Var *>(expr->data);
    auto constant = find_const(var->var.id_value);
    if (constant.has_value())
      return constant->expr;

    return std::unexpected(
        Error(std::format("Use undeclared constant `{}`", var->var.id_value),
              expr->start, expr->end));
  }

  case EXPR_BINARY: {
    auto binary = std::get<expr::Binary *>(expr->data);

    Expr *lhs = try$(evaluate_constant(binary->lhs, scope));
    Expr *rhs = try$(evaluate_constant(binary->rhs, scope));

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
    }
    std::unreachable();
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
}

std::optional<CheckedProc> Checker::find_proc(std::string_view name) {
  for (const auto &proc: _procs) {
    if (proc.name == name)
      return proc;
  }

  return std::nullopt;
}

std::optional<CheckedConst> Checker::find_const(std::string_view name) {
  for (const auto &const_: _consts) {
    if (const_.name == name)
      return const_;
  }

  return std::nullopt;
}
