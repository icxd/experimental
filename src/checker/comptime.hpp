#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include <checker/checker_types.hpp>
#include <parser/ast.hpp>

enum class ComptimeValueKind {
  Int,
  Bool,
  String,
  Stmt,
  Decl,
};

struct ComptimeValue {
  ComptimeValueKind kind = ComptimeValueKind::Int;
  int64_t int_value = 0;
  bool bool_value = false;
  std::string string_value;
  Stmt *stmt = nullptr;
  Decl *decl = nullptr;
};

struct ComptimeVar {
  Type *type = nullptr;
  ComptimeValue value;
};

struct ComptimeEnv {
  std::unordered_map<std::string, ComptimeVar> vars = {};
};

struct ComptimeProcInfo {
  decl::Proc *proc = nullptr;
  std::vector<CheckedParam> params;
  Type *ret_type = nullptr;
  bool intrinsic = false;
};

enum class ComptimeExpansionTarget {
  Module,
  StmtList,
};

struct ComptimeExpansion {
  ComptimeExpansionTarget target = ComptimeExpansionTarget::Module;
  std::vector<Decl *> injected_decls{};
  std::vector<Stmt *> injected_stmts{};
};
