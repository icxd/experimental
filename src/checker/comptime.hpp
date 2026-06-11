#pragma once

#include <string>
#include <unordered_map>

#include <checker/checker_types.hpp>
#include <parser/ast.hpp>

struct ComptimeVar {
  Type *type;
  Expr *value;
};

struct ComptimeEnv {
  std::unordered_map<std::string, ComptimeVar> vars = {};
};

struct ComptimeProcInfo {
  decl::Proc *proc;
  std::vector<CheckedParam> params;
  Type *ret_type;
};
