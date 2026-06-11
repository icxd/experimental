#pragma once

#include <limits>

#include <common.hpp>
#include <parser/ast.hpp>
#include <vector>

struct Scope;

struct CheckedParam {
  std::string_view name;
  Type *type;
};

struct CheckedProc {
  std::string_view name;
  std::vector<CheckedParam> params;
  Type *ret_type;
  Scope *scope;
};

struct CheckedConst {
  std::string_view name;
  Type *type;
  Expr *expr;
};

struct Scope {
  std::optional<Scope *> parent = std::nullopt;
  std::map<std::string_view, Type *> vars = {};

public:
  static Scope *create(std::optional<Scope *> parent = std::nullopt) {
    Scope *scope = new Scope;
    scope->parent = parent;
    return scope;
  }

  std::optional<Type *> find_var(std::string_view name) {
    if (vars.contains(name))
      return vars.at(name);

    if (parent.has_value())
      return parent.value()->find_var(name);

    return std::nullopt;
  }
};

class Checker {
public:
  ErrorOr<void> check_decls(const std::vector<Decl *> &decls);

private:
  ErrorOr<void> check_decl(Decl *decl, Scope *scope);
  ErrorOr<void> check_stmt(Stmt *stmt, Scope *scope);
  ErrorOr<Type *> check_expr(Expr *expr, Scope *scope);
  ErrorOr<Type *> check_type(Type *type, Scope *scope);

  ErrorOr<Expr *> evaluate_constant(Expr *expr, Scope *scope);

  bool type_eq(Type *a, Type *b);

  std::optional<CheckedProc> find_proc(std::string_view name);
  std::optional<CheckedConst> find_const(std::string_view name);

private:
  Scope *_global_scope = new Scope;
  std::vector<CheckedProc> _procs = {};
  size_t _current_proc_id = std::numeric_limits<size_t>::max();
  std::vector<CheckedConst> _consts = {};
};

static inline std::optional<std::pair<std::string, std::string>>
suggest_fix(Type *expected, Type *actual, Expr *expr) {
  if (expected->is_pointer_to(actual) && expr->is_lvalue())
    return std::make_pair("&" + expr->to_string(),
                          "Consider returning a pointer by using `&`");
  if (actual->is_pointer_to(expected))
    return std::make_pair("*" + expr->to_string(),
                          "Consider dereferencing the pointer by using `*`");
  // if (expected->is_string() && actual->is_int()) {
  //   return "\"" + expr->to_string() + "\"";
  // }
  return std::nullopt;
}
