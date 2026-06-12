#pragma once

#include <map>
#include <optional>
#include <string_view>
#include <vector>

#include <parser/ast.hpp>

struct Scope {
  std::optional<Scope *> parent = std::nullopt;
  std::map<std::string_view, Type *> vars = {};

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

struct CheckedParam {
  std::string_view name;
  Type *type;
};

struct CheckedProc {
  std::string_view name;
  std::vector<CheckedParam> params;
  Type *ret_type;
  Scope *scope;
  Linkage linkage = LINK_INTERN;
};

struct CheckedConst {
  std::string_view name;
  Type *type;
  Expr *expr;
};

struct CheckedStructField {
  std::string_view name;
  Type *type;
  size_t offset;
};

struct CheckedStruct {
  std::string_view name;
  std::vector<CheckedStructField> fields;
  size_t size;
};
