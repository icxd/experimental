#pragma once

#include <deque>

#include <checker/checker.hpp>
#include <checker/module_registry.hpp>
#include <common.hpp>
#include <ir/ir.hpp>
#include <parser/ast.hpp>

class Generator {
public:
  Generator(const std::vector<Decl *> &decls, std::string_view module_name,
            bool mangle_symbols, const ModuleRegistry *registry = nullptr,
            const std::vector<std::string> &imports = {},
            const std::vector<CheckedLambda> &lambdas = {}) :
      _decls(decls),
      _module_name(module_name),
      _mangle_symbols(mangle_symbols),
      _registry(registry),
      _imports(imports),
      _lambdas(lambdas) {}

  void gen_decls();

  Builder builder() const { return _builder; }

private:
  struct LoopLabels {
    std::string break_label;
    std::string continue_label;
  };

  void gen_decl(Decl *decl);
  void gen_lambda(const CheckedLambda &lambda);
  void gen_stmt(Stmt *stmt, Function *fn);
  Operand gen_expr(Expr *expr, Function *fn);
  void init_struct_var(Function *fn, const std::string &name, Type *type,
                       Expr *value);
  void init_aggregate_var(Function *fn, const std::string &name, Type *type,
                          Expr *value);
  void store_struct_fields(Function *fn, Operand base, Expr *value);
  void copy_aggregate(Function *fn, Operand dst, Operand src, size_t size);
  std::optional<CheckedStruct> find_struct(std::string_view name);
  size_t struct_size(Type *type);
  size_t tuple_size(Type *type);
  size_t aggregate_size(Type *type);
  size_t type_size(Type *type);
  size_t tuple_element_offset(Type *tuple_type, size_t index);
  Operand gen_index_addr(expr::Index *index, Function *fn);
  Operand gen_tuple_element_addr(expr::Index *index, Function *fn);
  size_t field_offset(expr::Field *field);
  Linkage find_proc_linkage(std::string_view module, std::string_view name);
  std::optional<CheckedProc> find_proc(std::string_view module,
                                       std::string_view name);
  Operand gen_call_arg(Expr *arg, Function *fn, Type *param_type);
  void store_lvalue(Function *fn, Expr *target, Operand src);
  std::string link_name(std::string_view module, std::string_view symbol,
                        Linkage linkage) const;
  std::string_view own_name(std::string name);

private:
  std::vector<Decl *> _decls;
  std::string _module_name;
  bool _mangle_symbols = true;
  const ModuleRegistry *_registry = nullptr;
  std::vector<std::string> _imports = {};
  std::vector<CheckedLambda> _lambdas = {};
  Builder _builder{};
  std::vector<LoopLabels> _loop_stack;
  std::deque<std::string> _owned_names;
  size_t _struct_tmp_counter = 0;
};
