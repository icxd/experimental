#include <algorithm>
#include <utility>

#include <checker/checker.hpp>
#include <host.hpp>
#include <module.hpp>
#include "parser/ast.hpp"

namespace fs = std::filesystem;

static bool block_always_returns(const std::vector<Stmt *> &stmts,
                                 bool needs_value);
static bool stmt_always_returns(Stmt *stmt, bool needs_value);

namespace {

std::string type_mangle_suffix(Type *type) {
  switch (type->type) {
  case TYPE_VOID: return "v";
  case TYPE_BOOL: return "b";
  case TYPE_INT:  return "i";
  case TYPE_BYTE: return "y";
  case TYPE_PTR: {
    Type *inner = std::get<type::Ptr *>(type->data)->inner;
    return "P" + type_mangle_suffix(inner);
  }
  case TYPE_STRUCT:
    return "S" + std::string(std::get<type::Struct *>(type->data)->name);
  case TYPE_PROC:
    return "F";
  case TYPE_TUPLE: {
    std::string out = "T";
    for (Type *element: std::get<type::Tuple *>(type->data)->elements)
      out += type_mangle_suffix(element);
    return out;
  }
  }
  std::unreachable();
}

std::string overload_codegen_name(std::string_view name,
                                  const std::vector<CheckedParam> &params) {
  std::string out(name);
  for (const auto &param: params)
    out += "$" + type_mangle_suffix(param.type);
  return out;
}

} // namespace

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
    return std::unexpected(boolean_condition_error(cond, cond_type));
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
    return std::unexpected(boolean_condition_error(cond, cond_type));
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

  for (Decl *decl: decls) {
    if (decl->type == DECL_STRUCT || decl->type == DECL_ENUM)
      try$(check_decl(decl, _global_scope));
  }

  for (size_t i = 0; i < decls.size();) {
    Decl *decl = decls[i];
    if (decl->type == DECL_STRUCT || decl->type == DECL_ENUM) {
      ++i;
      continue;
    }
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
      return std::unexpected(type_mismatch_error(
          std::format("Constant `{}` type mismatch", const_->name.id_value),
          type, value_type, value));
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

    if (find_comptime_proc(proc->name.id_value).has_value()) {
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

    for (const auto &existing: _procs) {
      if (existing.name == proc->name.id_value &&
          proc_signature_eq(params, ret_type, existing)) {
        return std::unexpected(Error(
            std::format("Redeclaration of procedure `{}`", proc->name.id_value),
            decl->start, decl->end));
      }
    }

    _procs.push_back(CheckedProc{
        .name = proc->name.id_value,
        .codegen_name = std::string(proc->name.id_value),
        .params = params,
        .ret_type = ret_type,
        .scope = proc_scope,
        .linkage = proc->linkage,
        .decl = proc,
    });
    refresh_codegen_names_for(proc->name.id_value);
    _current_proc_id = _procs.size() - 1;

    try$(expand_when_stmts(proc->body, proc_scope));
    for (auto *stmt: proc->body)
      try$(check_stmt(stmt, proc_scope));

    if (proc->linkage == LINK_INTERN && ret_type->type != TYPE_VOID &&
        !block_always_returns(proc->body, true)) {
      size_t end = proc->body.empty() ? proc->name.end : proc->body.back()->end;
      return std::unexpected(
          Error("Function must return a value on all paths", proc->name.start,
                end)
              .with_hint(std::format("Expected a value of type `{}`",
                                     ret_type->to_string())));
    }

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

  case DECL_STRUCT: {
    decl::Struct *strukt = std::get<decl::Struct *>(decl->data);
    return check_struct(strukt, decl->start, decl->end);
  }

  case DECL_ENUM: {
    decl::Enum *enum_ = std::get<decl::Enum *>(decl->data);
    return check_enum(enum_, decl->start, decl->end);
  }
  }
  std::unreachable();
}

ErrorOr<void> Checker::check_struct(decl::Struct *strukt, size_t start,
                                    size_t end) {
  std::string_view name = strukt->name.id_value;
  if (find_local_struct(name).has_value() ||
      find_local_enum(name).has_value()) {
    return std::unexpected(
        Error(std::format("Redeclaration of struct `{}`", name), start, end));
  }

  std::set<std::string_view> field_names{};
  std::vector<CheckedStructField> fields{};
  size_t offset = 0;
  for (const decl::StructField &field: strukt->fields) {
    std::string_view field_name = field.name.id_value;
    if (field_names.contains(field_name)) {
      return std::unexpected(
          Error(std::format("Duplicate field `{}` in struct `{}`", field_name,
                            name),
                field.name.start, field.name.end));
    }

    Type *field_type = try$(check_type(field.type, _global_scope));
    const_cast<decl::StructField &>(field).type = field_type;
    field_names.insert(field_name);
    fields.push_back(CheckedStructField{field_name, field_type, offset});
    offset += type_size(field_type);
  }

  _structs.push_back(CheckedStruct{name, fields, offset});
  return {};
}

ErrorOr<void> Checker::check_enum(decl::Enum *enum_, size_t start, size_t end) {
  std::string_view name = enum_->name.id_value;
  if (find_local_enum(name).has_value() ||
      find_local_struct(name).has_value()) {
    return std::unexpected(
        Error(std::format("Redeclaration of enum `{}`", name), start, end));
  }

  Type *underlying = nullptr;
  if (enum_->underlying != nullptr) {
    underlying = try$(check_type(enum_->underlying, _global_scope));
    if (underlying->type != TYPE_INT && underlying->type != TYPE_BYTE) {
      return std::unexpected(
          Error("Enum underlying type must be `int` or `byte`", start, end));
    }
  } else {
    underlying = new Type{TYPE_INT, start, end};
  }

  std::set<std::string_view> member_names{};
  std::vector<CheckedEnumMember> members{};
  int64_t next_value = 0;
  for (const decl::EnumMember &member: enum_->members) {
    std::string_view member_name = member.name.id_value;
    if (member_names.contains(member_name)) {
      return std::unexpected(
          Error(std::format("Duplicate enum member `{}` in `{}`", member_name,
                            name),
                member.name.start, member.name.end));
    }
    member_names.insert(member_name);

    if (member.value.has_value()) {
      Expr *value = try$(evaluate_constant(member.value.value(), _global_scope));
      Type *value_type = try$(check_expr(value, _global_scope));
      if (!type_eq(value_type, underlying)) {
        if (underlying->type == TYPE_BYTE && value_type->type == TYPE_INT &&
            value->type == EXPR_INT) {
          int64_t int_value = std::get<expr::Int *>(value->data)->value;
          if (int_value < 0 || int_value > 255) {
            return std::unexpected(
                Error("Enum member value out of range for `byte`",
                      member.value.value()->start, member.value.value()->end)
                    .with_hint("Expected a value between 0 and 255"));
          }
        } else {
          return std::unexpected(
              type_mismatch_error("Enum member value type mismatch", underlying,
                                  value_type, value));
        }
      }
      if (value->type != EXPR_INT) {
        return std::unexpected(
            Error("Enum member value must be an integer constant",
                  member.value.value()->start, member.value.value()->end));
      }
      next_value = std::get<expr::Int *>(value->data)->value;
    }

    members.push_back(CheckedEnumMember{member_name, next_value});
    next_value++;
  }

  members.push_back(
      CheckedEnumMember{"count", static_cast<int64_t>(enum_->members.size())});

  _enums.push_back(CheckedEnum{name, underlying, members});
  return {};
}

Type *Checker::make_enum_type(std::string_view name, size_t start, size_t end) {
  return new Type{
      .type = TYPE_ENUM,
      .start = start,
      .end = end,
      .data = new type::Enum{type::Enum{.name = name}},
  };
}

bool Checker::union_accepts_type(Type *union_type, Type *member_type) const {
  if (union_type->type != TYPE_UNION)
    return false;
  auto *union_data = std::get<type::Union *>(union_type->data);
  for (Type *member: union_data->members) {
    if (type_eq(member, member_type))
      return true;
  }
  return false;
}

size_t Checker::type_size(Type *type) {
  switch (type->type) {
  case TYPE_VOID: return 0;
  case TYPE_BOOL:
  case TYPE_INT:
  case TYPE_BYTE:
  case TYPE_PTR:  return 8;
  case TYPE_STRUCT: {
    std::string_view name = std::get<type::Struct *>(type->data)->name;
    if (auto local = find_local_struct(name); local.has_value())
      return local->size;
    return 8;
  }
  case TYPE_ENUM: {
    std::string_view name = std::get<type::Enum *>(type->data)->name;
    if (auto local = find_local_enum(name); local.has_value())
      return type_size(local->underlying_type);
    return 8;
  }
  case TYPE_UNION: {
    size_t max_size = 0;
    for (Type *member: std::get<type::Union *>(type->data)->members)
      max_size = std::max(max_size, type_size(member));
    return max_size;
  }
  case TYPE_PROC: return 8;
  case TYPE_TUPLE: {
    size_t size = 0;
    for (Type *element: std::get<type::Tuple *>(type->data)->elements)
      size += type_size(element);
    return size;
  }
  }
  std::unreachable();
}

Type *Checker::make_proc_type(const std::vector<CheckedParam> &params,
                              Type *ret_type, size_t start, size_t end) {
  std::vector<Param> ast_params{};
  for (const auto &param: params) {
    Token name{};
    name.id_value = std::string(param.name);
    ast_params.push_back(Param{name, param.type});
  }
  auto *proc_data =
      new type::Proc{type::Proc{.params = ast_params, .ret_type = ret_type}};
  return new Type{
      .type = TYPE_PROC,
      .start = start,
      .end = end,
      .data = proc_data,
  };
}

std::optional<Type *> Checker::proc_sig_type(Type *type) {
  if (type->type == TYPE_PROC)
    return type;
  return std::nullopt;
}

std::optional<CheckedStruct> Checker::find_local_struct(std::string_view name) {
  for (const auto &strukt: _structs) {
    if (strukt.name == name)
      return strukt;
  }
  return std::nullopt;
}

ErrorOr<CheckedStruct> Checker::find_prelude_struct(std::string_view name,
                                                      size_t start,
                                                      size_t end) {
  if (_registry == nullptr) {
    return std::unexpected(
        Error(std::format("Use of undeclared type `{}`", name), start, end));
  }

  for (const auto &[module_name, module]: _registry->modules()) {
    if (!is_prelude_module(module.path))
      continue;
    auto strukt = _registry->find_struct(module_name, name);
    if (strukt.has_value())
      return strukt.value();
  }

  return std::unexpected(
      Error(std::format("Use of undeclared type `{}`", name), start, end));
}

ErrorOr<CheckedStruct> Checker::find_imported_struct(std::string_view name,
                                                     size_t start,
                                                     size_t end) {
  if (_registry == nullptr || _imports.empty()) {
    return std::unexpected(
        Error(std::format("Use of undeclared type `{}`", name), start, end));
  }

  std::optional<CheckedStruct> found;
  for (const auto &import_module: _imports) {
    auto strukt = _registry->find_struct(import_module, name);
    if (!strukt.has_value())
      continue;
    if (found.has_value()) {
      return std::unexpected(
          Error(std::format("Ambiguous import for type `{}`", name), start,
                end)
              .with_hint("Multiple imported modules export a struct with this "
                         "name; define the type locally"));
    }
    found = strukt;
  }

  if (!found.has_value()) {
    return std::unexpected(
        Error(std::format("Use of undeclared type `{}`", name), start, end));
  }

  return *found;
}

ErrorOr<CheckedStruct> Checker::resolve_struct(std::string_view name,
                                               size_t start, size_t end) {
  if (auto local = find_local_struct(name); local.has_value())
    return *local;

  if (auto prelude = find_prelude_struct(name, start, end); prelude.has_value())
    return prelude.value();

  return find_imported_struct(name, start, end);
}

std::optional<CheckedEnum> Checker::find_local_enum(std::string_view name) {
  for (const auto &enum_: _enums) {
    if (enum_.name == name)
      return enum_;
  }
  return std::nullopt;
}

ErrorOr<CheckedEnum> Checker::find_imported_enum(std::string_view name,
                                                 size_t start, size_t end) {
  if (_registry == nullptr || _imports.empty()) {
    return std::unexpected(
        Error(std::format("Use of undeclared enum `{}`", name), start, end));
  }

  std::optional<CheckedEnum> found;
  for (const auto &import_module: _imports) {
    auto enum_ = _registry->find_enum(import_module, name);
    if (!enum_.has_value())
      continue;
    if (found.has_value()) {
      return std::unexpected(
          Error(std::format("Ambiguous import for enum `{}`", name), start, end)
              .with_hint("Multiple imported modules export an enum with this "
                         "name; define the type locally"));
    }
    found = enum_;
  }

  if (!found.has_value()) {
    return std::unexpected(
        Error(std::format("Use of undeclared enum `{}`", name), start, end));
  }

  return *found;
}

ErrorOr<CheckedEnum> Checker::resolve_enum(std::string_view name, size_t start,
                                           size_t end) {
  if (auto local = find_local_enum(name); local.has_value())
    return *local;
  return find_imported_enum(name, start, end);
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
        Error(std::format("Module `{}` is not available", import_module),
              start, end)
            .with_hint(std::format(
                "Import `{}` resolves to `{}`; imported modules must compile "
                "before the importer",
                import->path.string_value, resolved->string())));
  }

  if (std::find(_imports.begin(), _imports.end(), import_module) ==
      _imports.end())
    _imports.push_back(import_module);

  return {};
}

static bool block_always_returns(const std::vector<Stmt *> &stmts,
                                 bool needs_value) {
  for (Stmt *stmt: stmts) {
    if (stmt_always_returns(stmt, needs_value))
      return true;
  }
  return false;
}

static bool stmt_always_returns(Stmt *stmt, bool needs_value) {
  switch (stmt->type) {
  case STMT_RETURN: {
    auto *ret = std::get<stmt::Return *>(stmt->data);
    if (needs_value)
      return ret->value.has_value();
    return true;
  }
  case STMT_BLOCK:
    return block_always_returns(std::get<stmt::Block *>(stmt->data)->stmts,
                                needs_value);
  case STMT_IF: {
    auto *iff = std::get<stmt::If *>(stmt->data);
    if (iff->else_block.empty())
      return false;
    return block_always_returns(iff->then_block, needs_value) &&
           block_always_returns(iff->else_block, needs_value);
  }
  default:
    return false;
  }
}

ErrorOr<void> Checker::check_stmt(Stmt *stmt, Scope *scope) {
  switch (stmt->type) {
  case STMT_BLOCK: {
    stmt::Block *block = std::get<stmt::Block *>(stmt->data);
    Scope *block_scope = block->scoped ? Scope::create(scope) : scope;
    try$(expand_when_stmts(block->stmts, block_scope));
    for (Stmt *inner: block->stmts)
      try$(check_stmt(inner, block_scope));
    return {};
  }

  case STMT_WHEN:
    PANIC("unreachable");

  case STMT_ASSIGN: {
    stmt::Assign *assign = std::get<stmt::Assign *>(stmt->data);
    Type *lvalue_type = try$(check_lvalue(assign->target, scope));
    Type *expr_type = try$(check_expr(assign->value, scope));
    if (!type_eq(lvalue_type, expr_type) &&
        !(lvalue_type->type == TYPE_PTR && is_null_pointer_literal(assign->value))) {
      return std::unexpected(
          type_mismatch_error("Type mismatch in assignment", lvalue_type,
                            expr_type, assign->value));
    }
    assign->value->expr_type = lvalue_type;
    assign->target->expr_type = lvalue_type;
    return {};
  }

  case STMT_WHILE: {
    stmt::While *while_ = std::get<stmt::While *>(stmt->data);
    Type *cond_type = try$(check_expr(while_->cond, scope));
    if (!type_eq(cond_type, new Type{TYPE_BOOL})) {
      return std::unexpected(boolean_condition_error(while_->cond, cond_type));
    }
    while_->cond->expr_type =
        new Type{TYPE_BOOL, while_->cond->start, while_->cond->end};
    _loop_depth++;
    Scope *body_scope = Scope::create(scope);
    for (Stmt *inner: while_->body)
      try$(check_stmt(inner, body_scope));
    _loop_depth--;
    return {};
  }

  case STMT_FOR: {
    stmt::For *for_ = std::get<stmt::For *>(stmt->data);
    if (for_->init->type != STMT_VAR && for_->init->type != STMT_ASSIGN &&
        for_->init->type != STMT_BLOCK) {
      return std::unexpected(
          Error("For-loop init must be a variable declaration or assignment",
                for_->init->start, for_->init->end));
    }
    if (for_->init->type == STMT_BLOCK) {
      auto *block = std::get<stmt::Block *>(for_->init->data);
      for (Stmt *inner: block->stmts) {
        if (inner->type != STMT_VAR) {
          return std::unexpected(
              Error("For-loop init must be a variable declaration or assignment",
                    inner->start, inner->end));
        }
        try$(check_stmt(inner, scope));
      }
    } else {
      try$(check_stmt(for_->init, scope));
    }
    if (for_->step->type != STMT_ASSIGN) {
      return std::unexpected(
          Error("For-loop step must be an assignment", for_->step->start,
                for_->step->end));
    }
    Type *cond_type = try$(check_expr(for_->cond, scope));
    if (!type_eq(cond_type, new Type{TYPE_BOOL})) {
      return std::unexpected(boolean_condition_error(for_->cond, cond_type));
    }
    for_->cond->expr_type =
        new Type{TYPE_BOOL, for_->cond->start, for_->cond->end};
    _loop_depth++;
    Scope *body_scope = Scope::create(scope);
    for (Stmt *inner: for_->body)
      try$(check_stmt(inner, body_scope));
    _loop_depth--;
    try$(check_stmt(for_->step, scope));
    return {};
  }

  case STMT_IF: {
    stmt::If *if_ = std::get<stmt::If *>(stmt->data);
    Type *cond_type = try$(check_expr(if_->cond, scope));
    if (!type_eq(cond_type, new Type{TYPE_BOOL})) {
      return std::unexpected(boolean_condition_error(if_->cond, cond_type));
    }
    if_->cond->expr_type = new Type{TYPE_BOOL, if_->cond->start, if_->cond->end};
    Scope *then_scope = Scope::create(scope);
    for (Stmt *inner: if_->then_block)
      try$(check_stmt(inner, then_scope));
    Scope *else_scope = Scope::create(scope);
    for (Stmt *inner: if_->else_block)
      try$(check_stmt(inner, else_scope));
    return {};
  }

  case STMT_VAR: {
    stmt::Var *var = std::get<stmt::Var *>(stmt->data);

    std::string_view name = var->name.id_value;
    Type *type = nullptr;

    if (var->type == nullptr) {
      if (!var->value.has_value()) {
        return std::unexpected(
            Error("Cannot infer variable type without an initializer", var->name.start,
                  var->name.end));
      }
      type = try$(check_expr(var->value.value(), scope));
      var->type = type;
      var->value.value()->expr_type = type;
    } else {
      type = try$(check_type(var->type, scope));
      var->type = type;

      if (var->value.has_value()) {
        Type *expr_type = try$(check_expr(var->value.value(), scope, type));

        if (!type_eq(type, expr_type) &&
            !union_accepts_type(type, expr_type)) {
          if (type->type == TYPE_BYTE && expr_type->type == TYPE_INT &&
              var->value.value()->type == EXPR_INT) {
            int64_t value =
                std::get<expr::Int *>(var->value.value()->data)->value;
            if (value < 0 || value > 255) {
              return std::unexpected(
                  Error("Byte literal out of range", var->value.value()->start,
                        var->value.value()->end)
                      .with_hint("Expected a value between 0 and 255"));
            }
            expr_type = type;
          } else {
            return std::unexpected(type_mismatch_error(
                "Type mismatch in initializer", type, expr_type,
                var->value.value()));
          }
        }

        var->value.value()->expr_type = type;
      }
    }

    scope->vars.insert({name, type});

    return {};
  }

  case STMT_RETURN: {
    stmt::Return *ret = std::get<stmt::Return *>(stmt->data);

    CheckedProc curr_proc = _procs.at(_current_proc_id);
    if (curr_proc.ret_type->type == TYPE_VOID && ret->value.has_value()) {
      return std::unexpected(
          Error("Cannot return a value from void function",
                ret->value.value()->start, ret->value.value()->end)
              .with_hint("This function is declared to return `void`"));
    }

    if (curr_proc.ret_type->type != TYPE_VOID && !ret->value.has_value()) {
      return std::unexpected(
          Error("Missing return value", stmt->start, stmt->end)
              .with_hint(std::format("Expected a value of type `{}`",
                                     curr_proc.ret_type->to_string())));
    }

    if (ret->value.has_value()) {
      Expr *value = ret->value.value();
      Type *type = try$(check_expr(value, scope));
      if (!type_eq(curr_proc.ret_type, type) &&
          !(curr_proc.ret_type->type == TYPE_PTR &&
            is_null_pointer_literal(value))) {
        if (curr_proc.ret_type->type == TYPE_INT && type->type == TYPE_BYTE)
          type = curr_proc.ret_type;
        else {
          auto error = type_mismatch_error("Return type mismatch",
                                           curr_proc.ret_type, type, value);
          return std::unexpected(error);
        }
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

  case STMT_EXPR: {
    auto *expr_stmt = std::get<stmt::ExprStmt *>(stmt->data);
    Type *type = try$(check_expr(expr_stmt->expr, scope));
    if (type->type != TYPE_VOID) {
      return std::unexpected(
          Error("Expected void expression", stmt->start, stmt->end)
              .with_hint("Only calls to `void` procedures may be used as "
                         "standalone statements"));
    }
    return {};
  }
  }
  std::unreachable();
}

ErrorOr<Type *> Checker::check_expr(Expr *expr, Scope *scope, Type *expected) {
  if (expr->type == EXPR_COMPTIME_CALL) {
    Expr *folded = try$(evaluate_constant(expr, scope, nullptr));
    expr->type = folded->type;
    expr->data = folded->data;
    if (expr->expr_type != nullptr)
      expr->expr_type = folded->expr_type;
    return check_expr(expr, scope, expected);
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

    if (find_local_enum(var->var.id_value).has_value()) {
      return std::unexpected(
          Error(std::format("Enum type `{}` cannot be used as a value",
                            var->var.id_value),
                expr->start, expr->end));
    }

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
    type = try$(check_expr(group->expr, scope, expected));
    group->expr->expr_type = type;
    break;
  }

  case EXPR_BINARY: {
    expr::Binary *binary = std::get<expr::Binary *>(expr->data);

    Type *lhs_type = try$(check_expr(binary->lhs, scope));
    binary->lhs->expr_type = lhs_type;

    Type *rhs_type = try$(check_expr(binary->rhs, scope));
    binary->rhs->expr_type = rhs_type;

    if (binary->op == expr::Binary::BINOP_PLUS ||
        binary->op == expr::Binary::BINOP_MINUS) {
      bool lhs_ptr = lhs_type->type == TYPE_PTR;
      bool rhs_ptr = rhs_type->type == TYPE_PTR;
      bool lhs_int = lhs_type->type == TYPE_INT || lhs_type->type == TYPE_BYTE;
      bool rhs_int = rhs_type->type == TYPE_INT || rhs_type->type == TYPE_BYTE;

      if (lhs_ptr && rhs_int) {
        type = lhs_type;
        break;
      }
      if (binary->op == expr::Binary::BINOP_PLUS && rhs_ptr && lhs_int) {
        type = rhs_type;
        break;
      }
    }

    if (!type_eq(lhs_type, rhs_type)) {
      bool comparison =
          binary->op == expr::Binary::BINOP_EQ ||
          binary->op == expr::Binary::BINOP_NEQ ||
          binary->op == expr::Binary::BINOP_LT ||
          binary->op == expr::Binary::BINOP_LTE ||
          binary->op == expr::Binary::BINOP_GT ||
          binary->op == expr::Binary::BINOP_GTE;
      bool enum_int_cmp = false;
      if (comparison) {
        if (lhs_type->type == TYPE_ENUM &&
            (rhs_type->type == TYPE_INT || rhs_type->type == TYPE_BYTE)) {
          std::string_view enum_name =
              std::get<type::Enum *>(lhs_type->data)->name;
          if (auto enum_ = find_local_enum(enum_name); enum_.has_value()) {
            if (enum_->underlying_type->type == TYPE_BYTE &&
                rhs_type->type == TYPE_INT)
              enum_int_cmp = true;
            else
              enum_int_cmp = type_eq(enum_->underlying_type, rhs_type);
          }
        } else if (rhs_type->type == TYPE_ENUM &&
                    (lhs_type->type == TYPE_INT || lhs_type->type == TYPE_BYTE)) {
          std::string_view enum_name =
              std::get<type::Enum *>(rhs_type->data)->name;
          if (auto enum_ = find_local_enum(enum_name); enum_.has_value()) {
            if (enum_->underlying_type->type == TYPE_BYTE &&
                lhs_type->type == TYPE_INT)
              enum_int_cmp = true;
            else
              enum_int_cmp = type_eq(enum_->underlying_type, lhs_type);
          }
        }
      }
      if (!enum_int_cmp) {
        return std::unexpected(
            Error("Invalid binary operation", expr->start, expr->end)
                .with_hint(std::format("Cannot apply operator `{}` to operands "
                                       "of type `{}` and `{}`",
                                       binop_to_string(binary->op),
                                       lhs_type->to_string(),
                                       rhs_type->to_string())));
      }
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
        Error error("Logical operators require boolean operands", expr->start,
                    expr->end);
        error.with_hint(std::format("Expected `bool`, found `{}` and `{}`",
                                    lhs_type->to_string(),
                                    rhs_type->to_string()));
        Type bool_type{TYPE_BOOL, expr->start, expr->end};
        attach_type_fix(error, &bool_type, lhs_type, binary->lhs);
        attach_type_fix(error, &bool_type, rhs_type, binary->rhs);
        return std::unexpected(error);
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
      return std::unexpected(boolean_condition_error(not_->expr, inner));
    }
    type = new Type{TYPE_BOOL, expr->start, expr->end};
    break;
  }

  case EXPR_REF: {
    expr::Ref *ref = std::get<expr::Ref *>(expr->data);
    if (ref->expr->type == EXPR_VAR) {
      auto *var = std::get<expr::Var *>(ref->expr->data);
      std::optional<CheckedProc> proc;
      if (var->module.has_value()) {
        proc = find_qualified_proc(var->module->id_value, var->var.id_value);
      } else {
        proc = find_local_proc(var->var.id_value);
        if (!proc.has_value()) {
          auto imported =
              find_imported_proc(var->var.id_value, expr->start, expr->end);
          if (imported.has_value())
            proc = imported->first;
        }
      }
      if (proc.has_value()) {
        type = make_proc_type(proc->params, proc->ret_type, expr->start,
                              expr->end);
        break;
      }
    }

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
      return std::unexpected(
          Error("Cannot dereference non-pointer expression", expr->start,
                expr->end)
              .with_hint(std::format("Expected a pointer, found `{}`",
                                     inner->to_string())));
    }

    type = std::get<type::Ptr *>(inner->data)->inner;
    break;
  }

  case EXPR_CALL: {
    expr::Call *call = std::get<expr::Call *>(expr->data);

    auto check_call_arg = [&](Expr *argument, Type *param_type)
        -> ErrorOr<Type *> {
      Type *arg_type = try$(check_expr(argument, scope));
      if (type_eq(param_type, arg_type))
        return arg_type;

      if (param_type->type == TYPE_PTR && arg_type->type == TYPE_STRUCT) {
        Type *inner = std::get<type::Ptr *>(param_type->data)->inner;
        if (type_eq(inner, arg_type)) {
          if (!argument->is_lvalue()) {
            return std::unexpected(
                Error("Cannot take address of non-lvalue expression",
                      argument->start, argument->end));
          }
          return arg_type;
        }
      }

      auto error = type_mismatch_error("Parameter type mismatch", param_type,
                                       arg_type, argument);
      return std::unexpected(error);
    };

    if (call->callee != nullptr) {
      if (call->callee->type == EXPR_VAR) {
        auto *var = std::get<expr::Var *>(call->callee->data);
        if (!var->module.has_value()) {
          if (auto found = scope->find_var(var->var.id_value); found.has_value()) {
            Type *callee_type = found.value();
            call->callee->expr_type = callee_type;
            std::optional<Type *> proc_sig = proc_sig_type(callee_type);
            if (!proc_sig.has_value()) {
              return std::unexpected(
                  Error(std::format("Variable `{}` is not callable",
                                    var->var.id_value),
                        expr->start, expr->end)
                      .with_hint(std::format("Expected `proc(...)`, found `{}`",
                                             callee_type->to_string())));
            }

            auto *sig = std::get<type::Proc *>(proc_sig.value()->data);
            if (call->arguments.size() != sig->params.size()) {
              return std::unexpected(
                  Error("Argument count mismatch for procedure call", expr->start,
                        expr->end)
                      .with_hint(std::format("Expected {} parameters, got {}",
                                             sig->params.size(),
                                             call->arguments.size())));
            }

            for (size_t i = 0; i < call->arguments.size(); i++) {
              Expr *argument = call->arguments[i];
              Type *param_type = try$(check_type(sig->params[i].type, scope));
              Type *arg_type = try$(check_call_arg(argument, param_type));
              argument->expr_type = arg_type;
            }

            type = try$(check_type(sig->ret_type, scope));
            break;
          }

          std::vector<CheckedProc> candidates =
              find_local_procs(var->var.id_value);
          std::optional<std::string> resolved_module;
          if (candidates.empty()) {
            auto imported = try$(find_imported_proc(var->var.id_value, expr->start,
                                                   expr->end));
            resolved_module = imported.second;
            candidates =
                find_qualified_procs(*resolved_module, var->var.id_value);
          }

          if (!candidates.empty()) {
            CheckedProc resolved = try$(resolve_proc_overload(
                candidates, expr, scope, expr->start, expr->end));
            call->callee = nullptr;
            call->name = var->var;
            if (resolved_module.has_value())
              call->resolved_module = *resolved_module;
            call->resolved_codegen_name = resolved.codegen_name;

            const size_t receiver_args =
                call->receiver != nullptr ? 1 : 0;
            const size_t expected_user_args =
                resolved.params.size() - receiver_args;
            if (call->arguments.size() < expected_user_args) {
              return std::unexpected(
                  Error(std::format("Too few arguments for `{}`",
                                    var->var.id_value),
                        expr->start, expr->end)
                      .with_hint(std::format("Expected {} parameters, got {}",
                                             resolved.params.size(),
                                             call->arguments.size() +
                                                 receiver_args)));
            } else if (call->arguments.size() > expected_user_args) {
              return std::unexpected(
                  Error(std::format("Too many arguments for `{}`",
                                    var->var.id_value),
                        expr->start, expr->end)
                      .with_hint(std::format("Expected {} parameters, got {}",
                                             resolved.params.size(),
                                             call->arguments.size() +
                                                 receiver_args)));
            }

            size_t param_index = 0;
            if (call->receiver != nullptr) {
              Type *param_type =
                  try$(check_type(resolved.params[0].type, scope));
              Type *arg_type =
                  try$(check_call_arg(call->receiver, param_type));
              call->receiver->expr_type = arg_type;
              param_index = 1;
            }

            for (size_t i = 0; i < call->arguments.size(); i++) {
              Expr *argument = call->arguments[i];
              Type *param_type = try$(check_type(
                  resolved.params[param_index + i].type, scope));
              Type *arg_type = try$(check_call_arg(argument, param_type));
              argument->expr_type = arg_type;
            }

            type = try$(check_type(resolved.ret_type, scope));
            break;
          }
        }
      }

      Type *callee_type = try$(check_expr(call->callee, scope));
      call->callee->expr_type = callee_type;
      std::optional<Type *> proc_sig = proc_sig_type(callee_type);
      if (!proc_sig.has_value()) {
        return std::unexpected(
            Error("Cannot call non-procedure value", expr->start, expr->end)
                .with_hint(std::format("Expected `proc(...)`, found `{}`",
                                       callee_type->to_string())));
      }

      auto *sig = std::get<type::Proc *>(proc_sig.value()->data);
      if (call->arguments.size() != sig->params.size()) {
        return std::unexpected(
            Error("Argument count mismatch for procedure call", expr->start,
                  expr->end)
                .with_hint(std::format("Expected {} parameters, got {}",
                                       sig->params.size(),
                                       call->arguments.size())));
      }

      for (size_t i = 0; i < call->arguments.size(); i++) {
        Expr *argument = call->arguments[i];
        Type *param_type = try$(check_type(sig->params[i].type, scope));
        Type *arg_type = try$(check_call_arg(argument, param_type));
        argument->expr_type = arg_type;
      }

      type = try$(check_type(sig->ret_type, scope));
      break;
    }

    if (!call->name.has_value()) {
      return std::unexpected(
          Error("Invalid call expression", expr->start, expr->end));
    }

    std::optional<CheckedProc> proc;
    std::string qualified_name;
    if (call->module.has_value()) {
      std::string_view module = call->module->id_value;
      qualified_name =
          std::format("{}:{}", module, call->name->id_value);
      CheckedProc resolved = try$(resolve_proc_overload(
          find_qualified_procs(module, call->name->id_value), expr, scope,
          expr->start, expr->end));
      proc = resolved;
      call->resolved_codegen_name = resolved.codegen_name;
    } else {
      qualified_name = std::string(call->name->id_value);
      std::vector<CheckedProc> candidates = find_local_procs(call->name->id_value);
      if (candidates.empty()) {
        std::optional<std::string> import_module;
        for (const auto &module: _imports) {
          if (find_qualified_procs(module, call->name->id_value).empty())
            continue;
          if (import_module.has_value()) {
            return std::unexpected(
                Error(std::format("Ambiguous import for procedure `{}`",
                                  call->name->id_value),
                      expr->start, expr->end)
                    .with_hint("Multiple imported modules export a procedure "
                               "with this name; use a qualified call"));
          }
          import_module = module;
        }
        if (!import_module.has_value()) {
          return std::unexpected(
              Error(std::format("Use undeclared procedure `{}`", qualified_name),
                    expr->start, expr->end));
        }
        CheckedProc resolved = try$(resolve_proc_overload(
            find_qualified_procs(*import_module, call->name->id_value), expr,
            scope, expr->start, expr->end));
        proc = resolved;
        call->resolved_module = import_module;
        call->resolved_codegen_name = resolved.codegen_name;
      } else {
        CheckedProc resolved = try$(resolve_proc_overload(candidates, expr, scope,
                                                         expr->start, expr->end));
        proc = resolved;
        if (proc->decl != nullptr)
          call->resolved_codegen_name = proc->decl->codegen_name;
      }
    }

    if (!proc.has_value()) {
      return std::unexpected(
          Error(std::format("Use undeclared procedure `{}`", qualified_name),
                expr->start, expr->end));
    }

    if (call->resolved_codegen_name == std::nullopt &&
        proc->decl != nullptr)
      call->resolved_codegen_name = proc->decl->codegen_name;

    const size_t receiver_args =
        call->receiver != nullptr ? 1 : 0;
    const size_t expected_user_args = proc->params.size() - receiver_args;

    if (call->arguments.size() < expected_user_args) {
      return std::unexpected(
          Error(std::format("Too few arguments for `{}`", qualified_name),
                expr->start, expr->end)
              .with_hint(std::format("Expected {} parameters, got {}",
                                     proc->params.size(),
                                     call->arguments.size() + receiver_args)));
    } else if (call->arguments.size() > expected_user_args) {
      return std::unexpected(
          Error(std::format("Too many arguments for `{}`", qualified_name),
                expr->start, expr->end)
              .with_hint(std::format("Expected {} parameters, got {}",
                                     proc->params.size(),
                                     call->arguments.size() + receiver_args)));
    }

    size_t param_index = 0;
    if (call->receiver != nullptr) {
      Type *param_type = try$(check_type(proc->params[0].type, scope));
      Type *arg_type = try$(check_call_arg(call->receiver, param_type));
      call->receiver->expr_type = arg_type;
      param_index = 1;
    }

    for (size_t i = 0; i < call->arguments.size(); i++) {
      Expr *argument = call->arguments[i];
      Type *param_type = try$(check_type(proc->params[param_index + i].type, scope));
      Type *arg_type = try$(check_call_arg(argument, param_type));
      argument->expr_type = arg_type;
    }

    type = try$(check_type(proc->ret_type, scope));
    break;
  }

  case EXPR_COMPTIME_CALL:
    std::unreachable();

  case EXPR_ENUM_CASE: {
    auto *case_ = std::get<expr::EnumCase *>(expr->data);
    if (expected == nullptr || expected->type != TYPE_ENUM) {
      return std::unexpected(
          Error("Enum case requires a known enum type context", expr->start,
                expr->end)
              .with_hint("Use `.MEMBER` where an enum value is expected, or "
                         "use a qualified name like `EnumName.MEMBER`"));
    }
    std::string_view enum_name =
        std::get<type::Enum *>(expected->data)->name;
    CheckedEnum enum_ = try$(resolve_enum(enum_name, expr->start, expr->end));
    for (const auto &member: enum_.members) {
      if (member.name == case_->member.id_value) {
        type = make_enum_type(enum_name, expr->start, expr->end);
        break;
      }
    }
    if (type == nullptr) {
      return std::unexpected(
          Error(std::format("Enum `{}` has no member `{}`", enum_name,
                            case_->member.id_value),
                case_->member.start, case_->member.end));
    }
    break;
  }

  case EXPR_FIELD: {
    expr::Field *field = std::get<expr::Field *>(expr->data);

    if (field->base->type == EXPR_VAR) {
      auto *var = std::get<expr::Var *>(field->base->data);
      if (!var->module.has_value()) {
        std::optional<CheckedEnum> enum_;
        if (auto local = find_local_enum(var->var.id_value); local.has_value())
          enum_ = local;
        else if (auto imported =
                     find_imported_enum(var->var.id_value, expr->start, expr->end);
                 imported.has_value())
          enum_ = imported.value();

        if (enum_.has_value()) {
          for (const auto &member: enum_->members) {
            if (member.name == field->field.id_value) {
              type = make_enum_type(enum_->name, expr->start, expr->end);
              field->base->expr_type =
                  make_enum_type(enum_->name, field->base->start, field->base->end);
              break;
            }
          }
          if (type != nullptr)
            break;
          return std::unexpected(
              Error(std::format("Enum `{}` has no member `{}`", enum_->name,
                                field->field.id_value),
                    field->field.start, field->field.end));
        }
      }
    }

    Type *base_type = try$(check_expr(field->base, scope));
    field->base->expr_type = base_type;

    Type *struct_type = base_type->as_struct_for_field_access();
    if (struct_type == nullptr) {
      return std::unexpected(
          Error(std::format("Cannot access field `{}` on non-struct",
                            field->field.id_value),
                expr->start, expr->end)
              .with_hint(std::format("Expected a struct or pointer to struct, "
                                     "found `{}`",
                                     base_type->to_string())));
    }

    std::string_view struct_name =
        std::get<type::Struct *>(struct_type->data)->name;
    CheckedStruct strukt = try$(resolve_struct(struct_name, expr->start, expr->end));

    for (const auto &struct_field: strukt.fields) {
      if (struct_field.name == field->field.id_value) {
        type = struct_field.type;
        break;
      }
    }

    if (type == nullptr) {
      return std::unexpected(
          Error(std::format("No field `{}` on `{}`", field->field.id_value,
                            struct_name),
                field->field.start, field->field.end));
    }
    break;
  }

  case EXPR_STRUCT_LIT: {
    expr::StructLit *lit = std::get<expr::StructLit *>(expr->data);
    CheckedStruct strukt =
        try$(resolve_struct(lit->type_name.id_value, expr->start, expr->end));

    std::set<std::string_view> provided{};
    for (const expr::StructLitField &field: lit->fields) {
      if (provided.contains(field.name.id_value)) {
        return std::unexpected(
            Error(std::format("Duplicate field `{}` in struct literal",
                              field.name.id_value),
                  field.name.start, field.name.end));
      }
      provided.insert(field.name.id_value);

      std::optional<Type *> expected_type = std::nullopt;
      for (const auto &struct_field: strukt.fields) {
        if (struct_field.name == field.name.id_value) {
          expected_type = struct_field.type;
          break;
        }
      }

      if (!expected_type.has_value()) {
        return std::unexpected(
            Error(std::format("Struct `{}` has no field `{}`", strukt.name,
                              field.name.id_value),
                  field.name.start, field.name.end));
      }

      Type *value_type = try$(check_expr(field.value, scope, *expected_type));
      field.value->expr_type = value_type;
      if (!type_eq(*expected_type, value_type) &&
          !union_accepts_type(*expected_type, value_type) &&
          !((*expected_type)->type == TYPE_PTR &&
            is_null_pointer_literal(field.value))) {
        Error error("Type mismatch in struct literal", field.value->start,
                    field.value->end);
        error.with_hint(std::format("Field `{}`: expected `{}`, found `{}`",
                                    field.name.id_value,
                                    (*expected_type)->to_string(),
                                    value_type->to_string()));
        attach_type_fix(error, *expected_type, value_type, field.value);
        return std::unexpected(error);
      }
    }

    for (const auto &struct_field: strukt.fields) {
      if (!provided.contains(struct_field.name)) {
        Error error(std::format("Missing field `{}` in struct literal",
                                struct_field.name),
                    expr->start, expr->end);
        error.add_help(
            std::format("Add field `{}` to the struct literal", struct_field.name),
            expr->start, expr->end);
        return std::unexpected(error);
      }
    }

    type = new Type{
        .type = TYPE_STRUCT,
        .start = expr->start,
        .end = expr->end,
        .data = new type::Struct{strukt.name},
    };
    break;
  }

  case EXPR_STRING: {
    CheckedStruct string_struct =
        try$(resolve_struct("String", expr->start, expr->end));
    type = new Type{
        .type = TYPE_STRUCT,
        .start = expr->start,
        .end = expr->end,
        .data = new type::Struct{string_struct.name},
    };
    break;
  }

  case EXPR_SIZEOF: {
    auto *sizeof_expr = std::get<expr::Sizeof *>(expr->data);
    Type *operand_type = try$(check_type(sizeof_expr->type, scope));
    size_t size = type_size(operand_type);
    expr->type = EXPR_INT;
    expr->data = new expr::Int{static_cast<int64_t>(size)};
    type = new Type{TYPE_INT, expr->start, expr->end};
    break;
  }

  case EXPR_CAST: {
    auto *cast_ = std::get<expr::Cast *>(expr->data);
    Type *target = try$(check_type(cast_->target, scope));
    Type *from = try$(check_expr(cast_->expr, scope));
    cast_->expr->expr_type = from;
    if (!types_are_castable(from, target)) {
      auto error = Error("Invalid cast", expr->start, expr->end)
                       .with_hint(std::format("Cannot cast `{}` to `{}`",
                                              from->to_string(),
                                              target->to_string()));
      attach_type_fix(error, target, from, cast_->expr);
      return std::unexpected(error);
    }
    type = target;
    break;
  }

  case EXPR_INDEX: {
    auto *index = std::get<expr::Index *>(expr->data);
    Type *base_type = try$(check_expr(index->base, scope));
    index->base->expr_type = base_type;

    Type *index_type = try$(check_expr(index->index, scope));
    index->index->expr_type = index_type;
    if (index_type->type != TYPE_INT && index_type->type != TYPE_BYTE) {
      Type int_type{TYPE_INT, index->index->start, index->index->end};
      return std::unexpected(type_mismatch_error("Index must be an integer",
                                                 &int_type, index_type,
                                                 index->index));
    }

    if (base_type->type == TYPE_TUPLE) {
      if (index->index->type != EXPR_INT) {
        return std::unexpected(
            Error("Tuple index must be a constant integer", expr->start,
                  expr->end));
      }
      int64_t idx = std::get<expr::Int *>(index->index->data)->value;
      auto *tuple = std::get<type::Tuple *>(base_type->data);
      if (idx < 0 || static_cast<size_t>(idx) >= tuple->elements.size()) {
        return std::unexpected(
            Error(std::format("Tuple index {} out of bounds", idx), expr->start,
                  expr->end));
      }
      type = tuple->elements[static_cast<size_t>(idx)];
      break;
    }

    if (base_type->type != TYPE_PTR) {
      auto error =
          Error("Index operator requires a pointer base", expr->start, expr->end)
              .with_hint(std::format("Expected a pointer, found `{}`",
                                     base_type->to_string()));
      attach_index_base_fix(error, index->base);
      return std::unexpected(error);
    }

    type = std::get<type::Ptr *>(base_type->data)->inner;
    break;
  }

  case EXPR_TUPLE: {
    auto *tuple = std::get<expr::TupleLit *>(expr->data);
    if (tuple->elements.size() < 2) {
      return std::unexpected(
          Error("Tuple literal must have at least two elements", expr->start,
                expr->end));
    }
    std::vector<Type *> element_types{};
    for (Expr *element: tuple->elements) {
      Type *element_type = try$(check_expr(element, scope));
      element->expr_type = element_type;
      element_types.push_back(element_type);
    }
    auto *tuple_type = new type::Tuple{type::Tuple{.elements = element_types}};
    type = new Type{
        .type = TYPE_TUPLE,
        .start = expr->start,
        .end = expr->end,
        .data = tuple_type,
    };
    break;
  }

  case EXPR_PROC_LIT: {
    auto *lit = std::get<expr::ProcLit *>(expr->data);
    Scope *lambda_scope = Scope::create(scope);

    std::set<std::string_view> param_names{};
    std::vector<CheckedParam> checked_params{};
    for (const Param &param: lit->params) {
      std::string_view name = param.name.id_value;
      if (param_names.contains(name)) {
        return std::unexpected(
            Error(std::format("Parameter with name `{}` already exists", name),
                  param.name.start, param.name.end));
      }
      Type *param_type = try$(check_type(param.type, scope));
      param_names.insert(name);
      checked_params.push_back({name, param_type});
      lambda_scope->vars.insert({name, param_type});
    }

    Type *ret_type = try$(check_type(lit->ret_type, lambda_scope));

    size_t saved_proc_id = _current_proc_id;
    _procs.push_back(CheckedProc{
        .name = "__lambda",
        .codegen_name = lit->codegen_name,
        .params = checked_params,
        .ret_type = ret_type,
        .scope = lambda_scope,
        .linkage = LINK_INTERN,
        .decl = nullptr,
    });
    _current_proc_id = _procs.size() - 1;

    try$(expand_when_stmts(lit->body, lambda_scope));
    for (auto *stmt: lit->body)
      try$(check_stmt(stmt, lambda_scope));

    if (ret_type->type != TYPE_VOID &&
        !block_always_returns(lit->body, true)) {
      size_t end = lit->body.empty() ? expr->end : lit->body.back()->end;
      _current_proc_id = saved_proc_id;
      _procs.pop_back();
      return std::unexpected(
          Error("Lambda must return a value on all paths", expr->start, end)
              .with_hint(std::format("Expected a value of type `{}`",
                                     ret_type->to_string())));
    }

    _current_proc_id = saved_proc_id;
    _procs.pop_back();

    std::string codegen_name =
        std::format("__lambda_{}", _lambda_counter++);
    lit->codegen_name = codegen_name;
    _lambdas.push_back(CheckedLambda{
        codegen_name,
        checked_params,
        ret_type,
        lit->body,
    });

    type = make_proc_type(checked_params, ret_type, expr->start, expr->end);
    break;
  }
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
  case TYPE_BYTE: return type;
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
  case TYPE_STRUCT: {
    auto *strukt = std::get<type::Struct *>(type->data);
    std::string_view name = strukt->name;
    if (find_local_enum(name).has_value())
      return make_enum_type(name, type->start, type->end);
    if (auto imported = find_imported_enum(name, type->start, type->end);
        imported.has_value())
      return make_enum_type(name, type->start, type->end);
    try$(resolve_struct(name, type->start, type->end));
    return type;
  }
  case TYPE_ENUM: {
    auto *enum_type = std::get<type::Enum *>(type->data);
    try$(resolve_enum(enum_type->name, type->start, type->end));
    return type;
  }
  case TYPE_UNION: {
    auto *union_type = std::get<type::Union *>(type->data);
    if (union_type->members.empty()) {
      return std::unexpected(
          Error("Union type must have at least one member", type->start,
                type->end));
    }
    for (Type *member: union_type->members)
      try$(check_type(member, scope));
    return type;
  }
  case TYPE_PROC: {
    auto *proc = std::get<type::Proc *>(type->data);
    for (const Param &param: proc->params)
      try$(check_type(param.type, scope));
    try$(check_type(proc->ret_type, scope));
    return type;
  }
  case TYPE_TUPLE: {
    auto *tuple = std::get<type::Tuple *>(type->data);
    if (tuple->elements.size() < 2) {
      return std::unexpected(
          Error("Tuple type must have at least two elements", type->start,
                type->end));
    }
    for (Type *element: tuple->elements)
      try$(check_type(element, scope));
    return type;
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

  case EXPR_CAST: {
    auto cast_ = std::get<expr::Cast *>(expr->data);
    try$(fold_expr(cast_->expr, scope, env));
    break;
  }

  case EXPR_INDEX: {
    auto index = std::get<expr::Index *>(expr->data);
    try$(fold_expr(index->base, scope, env));
    try$(fold_expr(index->index, scope, env));
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
        Error(std::format("Argument count mismatch for `{}`",
                          proc->name.id_value),
              start, end)
            .with_hint(std::format("Expected {} arguments, got {}",
                                   info->params.size(), args.size())));
  }

  ComptimeEnv env{};
  for (size_t i = 0; i < args.size(); ++i) {
    Expr *arg = args[i];
    Type *param_type = info->params[i].type;
    Type *arg_type = try$(check_expr(arg, scope));
    if (!type_eq(param_type, arg_type)) {
      return std::unexpected(
          type_mismatch_error("Comptime argument type mismatch", param_type,
                              arg_type, arg));
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
      if (var->type == nullptr) {
        return std::unexpected(
            Error("Comptime variables must have an explicit type", stmt->start,
                  stmt->end));
      }
      Type *type = try$(check_type(var->type, scope));
      if (type->type != TYPE_INT && type->type != TYPE_BOOL) {
        return std::unexpected(
            Error("Comptime variables must have type `int` or `bool`",
                  stmt->start, stmt->end));
      }
      if (!var->value.has_value()) {
        return std::unexpected(
            Error("Comptime variables must have an initializer", stmt->start,
                  stmt->end));
      }
      Expr *value = try$(evaluate_constant(var->value.value(), scope, &env));
      Type *value_type = try$(check_expr(value, scope));
      if (!type_eq(type, value_type)) {
        return std::unexpected(
            type_mismatch_error("Comptime variable type mismatch", type,
                                value_type, value));
      }
      env.vars[std::string(var->name.id_value)] = {type, value};
      break;
    }

    case STMT_ASSIGN: {
      auto assign = std::get<stmt::Assign *>(stmt->data);
      if (assign->target->type != EXPR_VAR) {
        return std::unexpected(
            Error("Comptime assignment target must be a variable", stmt->start,
                  stmt->end));
      }
      auto *var = std::get<expr::Var *>(assign->target->data);
      std::string name(var->var.id_value);
      auto found = env.vars.find(name);
      if (found == env.vars.end()) {
        return std::unexpected(
            Error(std::format("Cannot assign to undeclared comptime variable "
                              "`{}`",
                              name),
                  assign->target->start, assign->target->end));
      }
      Expr *value = try$(evaluate_constant(assign->value, scope, &env));
      Type *value_type = try$(check_expr(value, scope));
      if (!type_eq(found->second.type, value_type)) {
        return std::unexpected(type_mismatch_error(
            "Comptime assignment type mismatch", found->second.type, value_type,
            assign->value));
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
            type_mismatch_error("Comptime return type mismatch", ret_type,
                                value_type, value));
      }
      return std::optional<Expr *>{value};
    }

    case STMT_IF: {
      auto if_ = std::get<stmt::If *>(stmt->data);
      Expr *cond = try$(evaluate_constant(if_->cond, scope, &env));
      Type *cond_type = try$(check_expr(cond, scope));
      if (!type_eq(cond_type, new Type{TYPE_BOOL})) {
        return std::unexpected(boolean_condition_error(if_->cond, cond_type));
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
              boolean_condition_error(while_->cond, cond_type));
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
    case STMT_EXPR:
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

bool Checker::type_eq(Type *a, Type *b) const {
  switch (a->type) {
  case TYPE_VOID: return b->type == TYPE_VOID;
  case TYPE_BOOL: return b->type == TYPE_BOOL;
  case TYPE_INT:  return b->type == TYPE_INT;
  case TYPE_BYTE: return b->type == TYPE_BYTE;
  case TYPE_PTR:  {
    Type *a_inner = std::get<type::Ptr *>(a->data)->inner;
    if (b->type != TYPE_PTR)
      return false;

    Type *b_inner = std::get<type::Ptr *>(b->data)->inner;
    return type_eq(a_inner, b_inner);
  }
  case TYPE_STRUCT: {
    if (b->type != TYPE_STRUCT)
      return false;
    auto *a_struct = std::get<type::Struct *>(a->data);
    auto *b_struct = std::get<type::Struct *>(b->data);
    return a_struct->name == b_struct->name;
  }
  case TYPE_ENUM: {
    if (b->type != TYPE_ENUM)
      return false;
    auto *a_enum = std::get<type::Enum *>(a->data);
    auto *b_enum = std::get<type::Enum *>(b->data);
    return a_enum->name == b_enum->name;
  }
  case TYPE_UNION: {
    if (b->type != TYPE_UNION)
      return false;
    auto *a_union = std::get<type::Union *>(a->data);
    auto *b_union = std::get<type::Union *>(b->data);
    if (a_union->members.size() != b_union->members.size())
      return false;
    for (size_t i = 0; i < a_union->members.size(); i++) {
      if (!type_eq(a_union->members[i], b_union->members[i]))
        return false;
    }
    return true;
  }
  case TYPE_PROC: {
    if (b->type != TYPE_PROC)
      return false;
    auto *a_proc = std::get<type::Proc *>(a->data);
    auto *b_proc = std::get<type::Proc *>(b->data);
    if (!type_eq(a_proc->ret_type, b_proc->ret_type))
      return false;
    if (a_proc->params.size() != b_proc->params.size())
      return false;
    for (size_t i = 0; i < a_proc->params.size(); i++) {
      if (!type_eq(a_proc->params[i].type, b_proc->params[i].type))
        return false;
    }
    return true;
  }
  case TYPE_TUPLE: {
    if (b->type != TYPE_TUPLE)
      return false;
    auto *a_tuple = std::get<type::Tuple *>(a->data);
    auto *b_tuple = std::get<type::Tuple *>(b->data);
    if (a_tuple->elements.size() != b_tuple->elements.size())
      return false;
    for (size_t i = 0; i < a_tuple->elements.size(); i++) {
      if (!type_eq(a_tuple->elements[i], b_tuple->elements[i]))
        return false;
    }
    return true;
  }
  }
  std::unreachable();
}

ErrorOr<Type *> Checker::check_lvalue(Expr *expr, Scope *scope) {
  switch (expr->type) {
  case EXPR_VAR: {
    auto *var = std::get<expr::Var *>(expr->data);
    if (var->module.has_value()) {
      return std::unexpected(
          Error("Cannot assign to qualified variable", expr->start, expr->end));
    }
    auto found = scope->find_var(var->var.id_value);
    if (!found.has_value()) {
      return std::unexpected(
          Error(std::format("Cannot assign to undeclared variable `{}`",
                            var->var.id_value),
                expr->start, expr->end));
    }
    return found.value();
  }

  case EXPR_FIELD: {
    auto *field = std::get<expr::Field *>(expr->data);
    Type *base_type = try$(check_expr(field->base, scope));
    field->base->expr_type = base_type;
    Type *struct_type = base_type->as_struct_for_field_access();
    if (struct_type == nullptr) {
      return std::unexpected(
          Error(std::format("Cannot assign to field of non-struct type `{}`",
                            base_type->to_string()),
                expr->start, expr->end)
              .with_hint("Expected a struct or pointer to struct"));
    }
    std::string_view struct_name =
        std::get<type::Struct *>(struct_type->data)->name;
    CheckedStruct strukt =
        try$(resolve_struct(struct_name, expr->start, expr->end));
    for (const auto &struct_field: strukt.fields) {
      if (struct_field.name == field->field.id_value)
        return struct_field.type;
    }
    return std::unexpected(
        Error(std::format("Struct `{}` has no field `{}`", struct_name,
                          field->field.id_value),
              field->field.start, field->field.end));
  }

  case EXPR_DEREF: {
    auto *deref = std::get<expr::Deref *>(expr->data);
    Type *inner = try$(check_expr(deref->expr, scope));
    if (inner->type != TYPE_PTR) {
      return std::unexpected(
          Error("Cannot dereference non-pointer in assignment", expr->start,
                expr->end));
    }
    return std::get<type::Ptr *>(inner->data)->inner;
  }

  case EXPR_INDEX: {
    auto *index = std::get<expr::Index *>(expr->data);
    Type *base_type = try$(check_expr(index->base, scope));
    index->base->expr_type = base_type;

    Type *index_type = try$(check_expr(index->index, scope));
    index->index->expr_type = index_type;
    if (index_type->type != TYPE_INT && index_type->type != TYPE_BYTE) {
      return std::unexpected(
          Error("Index must be an integer", expr->start, expr->end));
    }

    if (base_type->type == TYPE_TUPLE) {
      if (index->index->type != EXPR_INT) {
        return std::unexpected(
            Error("Tuple index must be a constant integer", expr->start,
                  expr->end));
      }
      int64_t idx = std::get<expr::Int *>(index->index->data)->value;
      auto *tuple = std::get<type::Tuple *>(base_type->data);
      if (idx < 0 || static_cast<size_t>(idx) >= tuple->elements.size()) {
        return std::unexpected(
            Error(std::format("Tuple index {} out of bounds", idx), expr->start,
                  expr->end));
      }
      return tuple->elements[static_cast<size_t>(idx)];
    }

    if (base_type->type != TYPE_PTR) {
      return std::unexpected(
          Error("Cannot assign through index on non-pointer", expr->start,
                expr->end));
    }

    return std::get<type::Ptr *>(base_type->data)->inner;
  }

  default:
    return std::unexpected(
        Error("Invalid assignment target", expr->start, expr->end)
            .with_hint("Only variables, fields, dereferenced pointers, and "
                       "indexed pointers can be assigned to"));
  }
}

std::optional<CheckedProc> Checker::find_local_proc(std::string_view name) {
  auto procs = find_local_procs(name);
  if (procs.empty())
    return std::nullopt;
  return procs.front();
}

std::vector<CheckedProc>
Checker::find_local_procs(std::string_view name) const {
  std::vector<CheckedProc> matches{};
  for (const auto &proc: _procs) {
    if (proc.name == name)
      matches.push_back(proc);
  }
  return matches;
}

std::optional<CheckedProc>
Checker::find_qualified_proc(std::string_view module, std::string_view name) {
  auto procs = find_qualified_procs(module, name);
  if (procs.empty())
    return std::nullopt;
  return procs.front();
}

std::vector<CheckedProc>
Checker::find_qualified_procs(std::string_view module,
                              std::string_view name) const {
  if (_registry == nullptr)
    return {};
  return _registry->find_procs(module, name);
}

bool Checker::proc_signature_eq(const std::vector<CheckedParam> &params,
                                Type *ret,
                                const CheckedProc &proc) const {
  if (!type_eq(ret, proc.ret_type))
    return false;
  if (params.size() != proc.params.size())
    return false;
  for (size_t i = 0; i < params.size(); i++) {
    if (!type_eq(params[i].type, proc.params[i].type))
      return false;
  }
  return true;
}

void Checker::refresh_codegen_names_for(std::string_view name) {
  std::vector<size_t> indices{};
  for (size_t i = 0; i < _procs.size(); i++) {
    if (_procs[i].name == name)
      indices.push_back(i);
  }
  if (indices.size() <= 1) {
    if (indices.size() == 1) {
      std::string plain(name);
      _procs[indices[0]].codegen_name = plain;
      if (_procs[indices[0]].decl != nullptr)
        _procs[indices[0]].decl->codegen_name = plain;
    }
    return;
  }

  for (size_t index: indices) {
    std::string mangled =
        overload_codegen_name(name, _procs[index].params);
    _procs[index].codegen_name = mangled;
    if (_procs[index].decl != nullptr)
      _procs[index].decl->codegen_name = mangled;
  }
}

bool Checker::call_arg_matches(Expr *argument, Type *param_type, Scope *scope) {
  auto result = check_expr(argument, scope);
  if (!result.has_value())
    return false;
  Type *arg_type = result.value();
  if (type_eq(param_type, arg_type))
    return true;
  if (param_type->type == TYPE_PTR && arg_type->type == TYPE_STRUCT &&
      argument->is_lvalue()) {
    Type *inner = std::get<type::Ptr *>(param_type->data)->inner;
    return type_eq(inner, arg_type);
  }
  return false;
}

ErrorOr<CheckedProc>
Checker::resolve_proc_overload(const std::vector<CheckedProc> &candidates,
                               Expr *call, Scope *scope, size_t start,
                               size_t end) {
  if (candidates.empty()) {
    return std::unexpected(
        Error("Use undeclared procedure", start, end));
  }

  auto *call_expr = std::get<expr::Call *>(call->data);
  const size_t receiver_args = call_expr->receiver != nullptr ? 1 : 0;
  const size_t user_args = call_expr->arguments.size();

  std::vector<const CheckedProc *> matches{};
  for (const CheckedProc &candidate: candidates) {
    const size_t expected_user_args = candidate.params.size() - receiver_args;
    if (user_args != expected_user_args)
      continue;

    bool ok = true;
    size_t param_index = 0;
    if (call_expr->receiver != nullptr) {
      if (!call_arg_matches(call_expr->receiver, candidate.params[0].type,
                            scope)) {
        ok = false;
      } else {
        param_index = 1;
      }
    }

    for (size_t i = 0; ok && i < call_expr->arguments.size(); i++) {
      if (!call_arg_matches(call_expr->arguments[i],
                            candidate.params[param_index + i].type, scope))
        ok = false;
    }

    if (ok)
      matches.push_back(&candidate);
  }

  if (matches.empty()) {
    return std::unexpected(
        Error("No matching overload for procedure call", start, end)
            .with_hint("Adjust argument types or use an explicit cast"));
  }
  if (matches.size() > 1) {
    return std::unexpected(
        Error("Ambiguous procedure overload", start, end)
            .with_hint("Multiple overloads match this call"));
  }

  return *matches.front();
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
