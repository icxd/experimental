#pragma once

#include <limits>

#include <checker/checker_types.hpp>
#include <checker/comptime.hpp>
#include <checker/module_registry.hpp>
#include <common.hpp>
#include <compile_config.hpp>
#include <parser/ast.hpp>
#include <vector>

class Checker {
public:
  Checker(std::string_view module_name, std::string_view file_path,
          const ModuleRegistry *registry = nullptr, bool is_runtime = false,
          const std::vector<std::string> &import_search_paths = {},
          const CompileConfig &config = {}) :
      _module_name(module_name),
      _file_path(file_path),
      _registry(registry),
      _is_runtime(is_runtime),
      _import_search_paths(import_search_paths),
      _config(config) {}

  ErrorOr<void> check_decls(std::vector<Decl *> &decls);

  const std::vector<CheckedProc> &procs() const { return _procs; }
  const std::vector<CheckedConst> &consts() const { return _consts; }
  const std::vector<CheckedStruct> &structs() const { return _structs; }
  const std::vector<std::string> &imports() const { return _imports; }

private:
  ErrorOr<void> check_decl(Decl *decl, Scope *scope);
  ErrorOr<void> check_stmt(Stmt *stmt, Scope *scope);
  ErrorOr<Type *> check_expr(Expr *expr, Scope *scope);
  ErrorOr<Type *> check_type(Type *type, Scope *scope);

  ErrorOr<Expr *> evaluate_constant(Expr *expr, Scope *scope,
                                    ComptimeEnv *env = nullptr);

  ErrorOr<void> fold_expr(Expr *expr, Scope *scope, ComptimeEnv *env = nullptr);

  ErrorOr<void> check_comptime_proc(decl::Proc *proc, Scope *scope);
  ErrorOr<Expr *> run_comptime_proc(decl::Proc *proc,
                                    const std::vector<Expr *> &args,
                                    Scope *scope, size_t start, size_t end);
  ErrorOr<std::optional<Expr *>>
  execute_comptime_stmts(const std::vector<Stmt *> &stmts, ComptimeEnv &env,
                         Scope *scope, Type *ret_type, size_t start,
                         size_t end);

  std::optional<ComptimeProcInfo> find_comptime_proc(std::string_view name);

  ErrorOr<void> inject_builtin_consts();
  ErrorOr<void> inject_define_consts();
  ErrorOr<std::vector<Decl *>> resolve_when_branch(const std::vector<Decl *> &block,
                                                   Scope *scope);
  ErrorOr<std::vector<Decl *>> resolve_when_decl(Decl *decl, Scope *scope);
  ErrorOr<std::vector<Stmt *>> resolve_when_branch_stmts(
      const std::vector<Stmt *> &block, Scope *scope);
  ErrorOr<std::vector<Stmt *>> resolve_when_stmt(Stmt *stmt, Scope *scope);
  ErrorOr<void> expand_when_stmts(std::vector<Stmt *> &stmts, Scope *scope);

  bool type_eq(Type *a, Type *b);

  std::optional<CheckedProc> find_local_proc(std::string_view name);
  std::optional<CheckedProc> find_qualified_proc(std::string_view module,
                                                 std::string_view name);
  ErrorOr<std::pair<CheckedProc, std::string>>
  find_imported_proc(std::string_view name, size_t start, size_t end);
  std::optional<CheckedConst> find_const(std::string_view name);
  std::optional<CheckedConst> find_qualified_const(std::string_view module,
                                                   std::string_view name);
  ErrorOr<CheckedConst> find_imported_const(std::string_view name, size_t start,
                                            size_t end);

  ErrorOr<void> check_import(decl::Import *import, size_t start, size_t end);
  ErrorOr<void> check_struct(decl::Struct *strukt, size_t start, size_t end);
  ErrorOr<CheckedStruct> resolve_struct(std::string_view name, size_t start,
                                        size_t end);
  std::optional<CheckedStruct> find_local_struct(std::string_view name);
  ErrorOr<CheckedStruct> find_imported_struct(std::string_view name,
                                              size_t start, size_t end);
  ErrorOr<CheckedStruct> find_prelude_struct(std::string_view name,
                                             size_t start, size_t end);
  size_t type_size(Type *type);

private:
  Scope *_global_scope = new Scope;
  std::string _module_name;
  std::string _file_path;
  const ModuleRegistry *_registry = nullptr;
  bool _is_runtime = false;
  std::vector<std::string> _import_search_paths = {};
  CompileConfig _config = {};
  std::vector<std::string> _imports = {};
  std::vector<CheckedProc> _procs = {};
  std::unordered_map<std::string, ComptimeProcInfo> _comptime_procs = {};
  size_t _current_proc_id = std::numeric_limits<size_t>::max();
  std::vector<CheckedConst> _consts = {};
  std::vector<CheckedStruct> _structs = {};
  size_t _loop_depth = 0;
};

static inline std::optional<std::pair<std::string, std::string>>
suggest_fix(Type *expected, Type *actual, Expr *expr) {
  if (expected->is_pointer_to(actual) && expr->is_lvalue())
    return std::make_pair("&" + expr->to_string(),
                          "Consider returning a pointer by using `&`");
  if (actual->is_pointer_to(expected))
    return std::make_pair("*" + expr->to_string(),
                          "Consider dereferencing the pointer by using `*`");
  return std::nullopt;
}
