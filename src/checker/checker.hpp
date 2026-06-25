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
  const std::vector<CheckedLambda> &lambdas() const { return _lambdas; }
  const std::vector<CheckedConst> &consts() const { return _consts; }
  const std::vector<CheckedStruct> &structs() const { return _structs; }
  const std::vector<CheckedEnum> &enums() const { return _enums; }
  const std::vector<std::string> &imports() const { return _imports; }

private:
  ErrorOr<void> check_decl(Decl *decl, Scope *scope);
  ErrorOr<void> check_stmt(Stmt *stmt, Scope *scope);
  ErrorOr<Type *> check_expr(Expr *expr, Scope *scope,
                             Type *expected = nullptr);
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

  ErrorOr<Type *> check_lvalue(Expr *expr, Scope *scope);
  bool type_eq(Type *a, Type *b) const;
  Type *make_proc_type(const std::vector<CheckedParam> &params, Type *ret_type,
                       size_t start, size_t end);
  std::optional<Type *> proc_sig_type(Type *type);

  std::optional<CheckedProc> find_local_proc(std::string_view name);
  std::vector<CheckedProc> find_local_procs(std::string_view name) const;
  std::optional<CheckedProc> find_qualified_proc(std::string_view module,
                                                 std::string_view name);
  std::vector<CheckedProc> find_qualified_procs(std::string_view module,
                                                std::string_view name) const;
  ErrorOr<std::pair<CheckedProc, std::string>>
  find_imported_proc(std::string_view name, size_t start, size_t end);
  ErrorOr<CheckedProc>
  resolve_proc_overload(const std::vector<CheckedProc> &candidates,
                        Expr *call, Scope *scope, size_t start, size_t end);
  bool proc_signature_eq(const std::vector<CheckedParam> &params, Type *ret,
                         const CheckedProc &proc) const;
  void refresh_codegen_names_for(std::string_view name);
  bool call_arg_matches(Expr *argument, Type *param_type, Scope *scope);
  std::optional<CheckedConst> find_const(std::string_view name);
  std::optional<CheckedConst> find_qualified_const(std::string_view module,
                                                   std::string_view name);
  ErrorOr<CheckedConst> find_imported_const(std::string_view name, size_t start,
                                            size_t end);

  ErrorOr<void> check_import(decl::Import *import, size_t start, size_t end);
  ErrorOr<void> check_struct(decl::Struct *strukt, size_t start, size_t end);
  ErrorOr<void> check_enum(decl::Enum *enum_, size_t start, size_t end);
  ErrorOr<CheckedStruct> resolve_struct(std::string_view name, size_t start,
                                        size_t end);
  std::optional<CheckedStruct> find_local_struct(std::string_view name);
  ErrorOr<CheckedStruct> find_imported_struct(std::string_view name,
                                              size_t start, size_t end);
  ErrorOr<CheckedStruct> find_prelude_struct(std::string_view name,
                                             size_t start, size_t end);
  ErrorOr<CheckedEnum> resolve_enum(std::string_view name, size_t start,
                                    size_t end);
  std::optional<CheckedEnum> find_local_enum(std::string_view name);
  ErrorOr<CheckedEnum> find_imported_enum(std::string_view name, size_t start,
                                          size_t end);
  ErrorOr<CheckedEnum> find_imported_enum_by_member(std::string_view member,
                                                    size_t start, size_t end);
  Type *make_enum_type(std::string_view name, size_t start, size_t end);
  bool union_accepts_type(Type *union_type, Type *member_type) const;
  size_t type_size(Type *type);

  Scope *_global_scope = new Scope;
  std::string _module_name;
  std::string _file_path;
  const ModuleRegistry *_registry = nullptr;
  bool _is_runtime = false;
  std::vector<std::string> _import_search_paths = {};
  CompileConfig _config = {};
  std::vector<std::string> _imports = {};
  std::vector<CheckedProc> _procs = {};
  std::vector<CheckedLambda> _lambdas = {};
  size_t _lambda_counter = 0;
  std::unordered_map<std::string, ComptimeProcInfo> _comptime_procs = {};
  size_t _current_proc_id = std::numeric_limits<size_t>::max();
  std::vector<CheckedConst> _consts = {};
  std::vector<CheckedStruct> _structs = {};
  std::vector<CheckedEnum> _enums = {};
  size_t _loop_depth = 0;
};

static inline bool is_null_pointer_literal(Expr *expr) {
  return expr->type == EXPR_INT &&
         std::get<expr::Int *>(expr->data)->value == 0;
}

static inline bool is_scalar_type(Type *type) {
  return type->type == TYPE_INT || type->type == TYPE_BYTE ||
         type->type == TYPE_BOOL;
}

static inline bool types_are_castable(Type *from, Type *to) {
  if (from->type == TYPE_VOID || to->type == TYPE_VOID)
    return false;
  if (from->type == TYPE_STRUCT || to->type == TYPE_STRUCT)
    return false;
  if (from->type == TYPE_PTR && to->type == TYPE_PTR)
    return true;
  if (is_scalar_type(from) && is_scalar_type(to))
    return true;
  if (from->type == TYPE_PTR && is_scalar_type(to))
    return true;
  if (is_scalar_type(from) && to->type == TYPE_PTR)
    return true;
  return false;
}

static inline std::optional<std::pair<std::string, std::string>>
suggest_fix(Type *expected, Type *actual, Expr *expr) {
  if (expected->is_pointer_to(actual) && expr->is_lvalue())
    return std::make_pair("&" + expr->to_string(),
                          "Take the address with `&`");
  if (actual->is_pointer_to(expected))
    return std::make_pair("*" + expr->to_string(),
                          "Dereference the pointer with `*`");
  if (types_are_castable(actual, expected))
    return std::make_pair(
        std::format("cast({}) {}", expected->to_string(), expr->to_string()),
        std::format("Try an explicit `cast({})`", expected->to_string()));
  if (expected->type == TYPE_BOOL &&
      (actual->type == TYPE_INT || actual->type == TYPE_BYTE))
    return std::make_pair(std::format("{} != 0", expr->to_string()),
                          "Compare against zero to produce a boolean");
  return std::nullopt;
}

static inline void attach_type_fix(Error &error, Type *expected, Type *actual,
                                 Expr *expr) {
  if (auto fix = suggest_fix(expected, actual, expr))
    error.add_help(ErrorHelp(*fix, expr->start, expr->end));
}

static inline void attach_index_base_fix(Error &error, Expr *base) {
  if (!base->is_lvalue())
    return;
  error.add_help(
      ErrorHelp({"&" + base->to_string(), "Take the address of the base value"},
                base->start, base->end));
}

static inline Error type_mismatch_error(std::string message, Type *expected,
                                        Type *actual, Expr *expr) {
  Error error(std::move(message), expr->start, expr->end);
  error.with_hint(std::format("Expected `{}`, found `{}`",
                              expected->to_string(), actual->to_string()));
  attach_type_fix(error, expected, actual, expr);
  return error;
}

static inline Error boolean_condition_error(Expr *cond, Type *cond_type) {
  Error error("Condition has to be a boolean", cond->start, cond->end);
  error.with_hint(
      std::format("Expected `bool`, found `{}`", cond_type->to_string()));
  attach_type_fix(error, new Type{TYPE_BOOL, cond->start, cond->end},
                  cond_type, cond);
  return error;
}
