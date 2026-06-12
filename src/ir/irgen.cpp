#include <ir/ir.hpp>
#include <ir/irgen.hpp>
#include <module.hpp>
#include <vector>
#include "parser/ast.hpp"

std::string Generator::link_name(std::string_view module,
                                 std::string_view symbol,
                                 Linkage linkage) const {
  return ::link_name(module, symbol, _mangle_symbols, linkage);
}

std::string_view Generator::own_name(std::string name) {
  _owned_names.push_back(std::move(name));
  return _owned_names.back();
}

void Generator::gen_decls() {
  for (auto *decl: _decls)
    gen_decl(decl);
}

void Generator::gen_decl(Decl *decl) {
  switch (decl->type) {
  case DECL_CONST: {
    auto const_ = std::get<decl::Const *>(decl->data);
    Operand value = gen_expr(const_->value, nullptr);
    _builder.create_constant(const_->name.id_value, value);
  } break;

  case DECL_PROC: {
    auto proc = std::get<decl::Proc *>(decl->data);
    if (proc->is_comptime)
      break;

    std::vector<std::string_view> params{};
    for (const auto &param: proc->params)
      params.push_back(param.name.id_value);

  std::string_view fn_name = own_name(
        link_name(_module_name, proc->name.id_value, proc->linkage));
    Function *fn = _builder.create_function(fn_name, params);
    fn->linkage = proc->linkage;
    for (const auto &stmt: proc->body)
      gen_stmt(stmt, fn);

    fn->instructions = fold_constants(fn->instructions);
    fn->instructions = fold_temporaries(fn->instructions);
  } break;

  case DECL_IMPORT: break;

  case DECL_STRUCT: break;

  case DECL_WHEN:
  case DECL_COMPTIME_BLOCK:
    PANIC("unreachable");
  }
}

void Generator::gen_stmt(Stmt *stmt, Function *fn) {
  switch (stmt->type) {
  case STMT_BLOCK: {
    auto block = std::get<stmt::Block *>(stmt->data);
    for (Stmt *inner: block->stmts)
      gen_stmt(inner, fn);
  } break;

  case STMT_VAR: {
    auto var = std::get<stmt::Var *>(stmt->data);
    std::string name(var->name.id_value);
    if (var->type->type == TYPE_STRUCT) {
      fn->variable_sizes[name] = struct_size(var->type);
      init_struct_var(fn, name, var->type, var->value);
    } else {
      fn->variable_sizes[name] = 8;
      Operand src = gen_expr(var->value, fn);
      fn->assign(Operand::Variable(name), src);
    }
  } break;

  case STMT_ASSIGN: {
    auto assign = std::get<stmt::Assign *>(stmt->data);
    Operand src = gen_expr(assign->value, fn);
    fn->assign(Operand::Variable(std::string(assign->name.id_value)), src);
  } break;

  case STMT_WHILE: {
    auto while_ = std::get<stmt::While *>(stmt->data);
    std::string loop = _builder.new_label();
    std::string end = _builder.new_label();
    _loop_stack.push_back(LoopLabels{.break_label = end, .continue_label = loop});
    fn->label(Operand::Label(loop));
    Operand cond = gen_expr(while_->cond, fn);
    fn->jmp_if_zero(cond, Operand::Label(end));
    for (Stmt *inner: while_->body)
      gen_stmt(inner, fn);
    fn->jmp(Operand::Label(loop));
    fn->label(Operand::Label(end));
    _loop_stack.pop_back();
  } break;

  case STMT_FOR: {
    auto for_ = std::get<stmt::For *>(stmt->data);
    std::string loop = _builder.new_label();
    std::string step = _builder.new_label();
    std::string end = _builder.new_label();
    gen_stmt(for_->init, fn);
    fn->label(Operand::Label(loop));
    Operand cond = gen_expr(for_->cond, fn);
    fn->jmp_if_zero(cond, Operand::Label(end));
    _loop_stack.push_back(LoopLabels{.break_label = end, .continue_label = step});
    for (Stmt *inner: for_->body)
      gen_stmt(inner, fn);
    _loop_stack.pop_back();
    fn->label(Operand::Label(step));
    gen_stmt(for_->step, fn);
    fn->jmp(Operand::Label(loop));
    fn->label(Operand::Label(end));
  } break;

  case STMT_IF: {
    auto if_ = std::get<stmt::If *>(stmt->data);
    Operand cond = gen_expr(if_->cond, fn);
    std::string end_label = _builder.new_label();

    if (!if_->else_block.empty()) {
      std::string else_label = _builder.new_label();
      fn->jmp_if_zero(cond, Operand::Label(else_label));
      for (Stmt *inner: if_->then_block)
        gen_stmt(inner, fn);
      fn->jmp(Operand::Label(end_label));
      fn->label(Operand::Label(else_label));
      for (Stmt *inner: if_->else_block)
        gen_stmt(inner, fn);
      fn->label(Operand::Label(end_label));
    } else {
      fn->jmp_if_zero(cond, Operand::Label(end_label));
      for (Stmt *inner: if_->then_block)
        gen_stmt(inner, fn);
      fn->label(Operand::Label(end_label));
    }
  } break;

  case STMT_RETURN: {
    auto ret = std::get<stmt::Return *>(stmt->data);
    if (ret->value.has_value()) {
      Operand value = gen_expr(ret->value.value(), fn);
      fn->ret(value);
    } else {
      fn->ret();
    }
  } break;

  case STMT_BREAK: {
    fn->jmp(Operand::Label(_loop_stack.back().break_label));
  } break;

  case STMT_CONTINUE: {
    fn->jmp(Operand::Label(_loop_stack.back().continue_label));
  } break;

  case STMT_WHEN:
    PANIC("unreachable");
  }
}

Operand Generator::gen_expr(Expr *expr, Function *fn) {
  switch (expr->type) {
  case EXPR_INT: {
    auto int_ = std::get<expr::Int *>(expr->data);
    return Operand::ConstantInt(int_->value);
  }

  case EXPR_BOOL: {
    auto bool_ = std::get<expr::Bool *>(expr->data);
    return Operand::ConstantInt(bool_->value ? 1 : 0);
  }

  case EXPR_VAR: {
    auto var = std::get<expr::Var *>(expr->data);

    if (var->module.has_value() && _registry != nullptr) {
      auto imported = _registry->find_const(var->module->id_value,
                                            var->var.id_value);
      if (imported.has_value())
        return gen_expr(imported->expr, fn);
    }

    if (!var->module.has_value() && _registry != nullptr) {
      for (const auto &import_module: _imports) {
        auto imported =
            _registry->find_const(import_module, var->var.id_value);
        if (imported.has_value())
          return gen_expr(imported->expr, fn);
      }
    }

    auto constant = _builder.find_constant(var->var.id_value);
    if (constant.has_value())
      return Operand::Constant(std::string(constant->name));

    return Operand::Variable(std::string(var->var.id_value));
  }

  case EXPR_GROUP: {
    auto group = std::get<expr::Group *>(expr->data);
    return gen_expr(group->expr, fn);
  }

  case EXPR_BINARY: {
    auto binary = std::get<expr::Binary *>(expr->data);

    if (binary->op == expr::Binary::BINOP_AND) {
      Operand lhs = gen_expr(binary->lhs, fn);
      std::string false_label = _builder.new_label();
      std::string end_label = _builder.new_label();
      Operand dst = _builder.new_temp();
      fn->jmp_if_zero(lhs, Operand::Label(false_label));
      Operand rhs = gen_expr(binary->rhs, fn);
      fn->assign(dst, rhs);
      fn->jmp(Operand::Label(end_label));
      fn->label(Operand::Label(false_label));
      fn->assign(dst, Operand::ConstantInt(0));
      fn->label(Operand::Label(end_label));
      return dst;
    }

    if (binary->op == expr::Binary::BINOP_OR) {
      Operand lhs = gen_expr(binary->lhs, fn);
      std::string true_label = _builder.new_label();
      std::string end_label = _builder.new_label();
      Operand dst = _builder.new_temp();
      fn->jmp_if_nonzero(lhs, Operand::Label(true_label));
      Operand rhs = gen_expr(binary->rhs, fn);
      fn->assign(dst, rhs);
      fn->jmp(Operand::Label(end_label));
      fn->label(Operand::Label(true_label));
      fn->assign(dst, Operand::ConstantInt(1));
      fn->label(Operand::Label(end_label));
      return dst;
    }

    Operand dst = _builder.new_temp();
    Operand lhs = gen_expr(binary->lhs, fn);
    Operand rhs = gen_expr(binary->rhs, fn);

    switch (binary->op) {
    case expr::Binary::BINOP_PLUS:  fn->add(dst, lhs, rhs); break;
    case expr::Binary::BINOP_MINUS: fn->sub(dst, lhs, rhs); break;
    case expr::Binary::BINOP_STAR:  fn->mul(dst, lhs, rhs); break;
    case expr::Binary::BINOP_SLASH: fn->div(dst, lhs, rhs); break;
    case expr::Binary::BINOP_EQ:    fn->cmp_eq(dst, lhs, rhs); break;
    case expr::Binary::BINOP_NEQ:   fn->cmp_neq(dst, lhs, rhs); break;
    case expr::Binary::BINOP_LT:    fn->cmp_lt(dst, lhs, rhs); break;
    case expr::Binary::BINOP_LTE:   fn->cmp_lte(dst, lhs, rhs); break;
    case expr::Binary::BINOP_GT:    fn->cmp_gt(dst, lhs, rhs); break;
    case expr::Binary::BINOP_GTE:   fn->cmp_gte(dst, lhs, rhs); break;
    case expr::Binary::BINOP_AND:
    case expr::Binary::BINOP_OR:    break;
    }

    return dst;
  }

  case EXPR_NOT: {
    auto not_ = std::get<expr::Not *>(expr->data);
    Operand src = gen_expr(not_->expr, fn);
    Operand dst = _builder.new_temp();
    fn->cmp_eq(dst, src, Operand::ConstantInt(0));
    return dst;
  }

  case EXPR_REF: {
    auto ref = std::get<expr::Ref *>(expr->data);
    Operand dst = _builder.new_temp();
    Operand src = gen_expr(ref->expr, fn);
    fn->addrof(dst, src);
    return dst;
  }

  case EXPR_DEREF: {
    auto deref = std::get<expr::Deref *>(expr->data);
    Operand dst = _builder.new_temp();
    Operand src = gen_expr(deref->expr, fn);
    fn->deref(dst, src);
    return dst;
  }

  case EXPR_COMPTIME_CALL:
    PANIC("unreachable");

  case EXPR_CALL: {
    auto call = std::get<expr::Call *>(expr->data);
    Operand dst = _builder.new_temp();
    std::string_view module = _module_name;
    if (call->module.has_value())
      module = call->module->id_value;
    else if (call->resolved_module.has_value())
      module = *call->resolved_module;
    Linkage linkage = find_proc_linkage(module, call->name.id_value);
    std::string callee = link_name(module, call->name.id_value, linkage);
    Operand name = Operand::Function(callee);
    std::vector<Operand> args{};
    for (auto *arg: call->arguments)
      args.push_back(gen_expr(arg, fn));
    fn->call(dst, name, args);
    return dst;
  }

  case EXPR_FIELD: {
    auto field = std::get<expr::Field *>(expr->data);
    Type *base_type = field->base->expr_type;
    std::string_view struct_name =
        std::get<type::Struct *>(base_type->data)->name;
    auto strukt = find_struct(struct_name);
    if (!strukt.has_value())
      PANIC("struct not found during codegen");

    size_t offset = 0;
    for (const auto &struct_field: strukt->fields) {
      if (struct_field.name == field->field.id_value) {
        offset = struct_field.offset;
        break;
      }
    }

    Operand base;
    Expr *base_expr = field->base;
    if (base_expr->type == EXPR_GROUP)
      base_expr = std::get<expr::Group *>(base_expr->data)->expr;

    if (base_expr->type == EXPR_VAR) {
      base = Operand::Variable(std::string(
          std::get<expr::Var *>(base_expr->data)->var.id_value));
    } else if (base_expr->type == EXPR_DEREF) {
      auto deref = std::get<expr::Deref *>(base_expr->data);
      Operand ptr = gen_expr(deref->expr, fn);
      Operand ptr_val = _builder.new_temp();
      fn->assign(ptr_val, ptr);
      Operand dst = _builder.new_temp();
      fn->load_offset(dst, ptr_val, static_cast<int64_t>(offset));
      return dst;
    } else {
      PANIC("field access only supported on variables and pointers");
    }

    Operand dst = _builder.new_temp();
    fn->load_offset(dst, base, static_cast<int64_t>(offset));
    return dst;
  }

  case EXPR_STRUCT_LIT: {
    std::string temp = std::format("__struct_tmp_{}", _struct_tmp_counter++);
    Type *type = expr->expr_type;
    fn->variable_sizes[temp] = struct_size(type);
    init_struct_var(fn, temp, type, expr);
    return Operand::Variable(temp);
  }

  case EXPR_STRING: {
    std::string temp = std::format("__string_tmp_{}", _struct_tmp_counter++);
    Type *type = expr->expr_type;
    fn->variable_sizes[temp] = struct_size(type);
    init_struct_var(fn, temp, type, expr);
    return Operand::Variable(temp);
  }
  }
  PANIC("unhandled expr type");
}

Linkage Generator::find_proc_linkage(std::string_view module,
                                     std::string_view name) {
  if (module == _module_name) {
    for (Decl *decl: _decls) {
      if (decl->type != DECL_PROC)
        continue;
      auto *proc = std::get<decl::Proc *>(decl->data);
      if (proc->name.id_value == name)
        return proc->linkage;
    }
  }

  if (_registry != nullptr) {
    auto proc = _registry->find_proc(module, name);
    if (proc.has_value())
      return LINK_INTERN;
  }

  return LINK_INTERN;
}

std::optional<CheckedStruct> Generator::find_struct(std::string_view name) {
  for (Decl *decl: _decls) {
    if (decl->type != DECL_STRUCT)
      continue;
    auto *strukt = std::get<decl::Struct *>(decl->data);
    if (strukt->name.id_value == name) {
      size_t offset = 0;
      std::vector<CheckedStructField> fields{};
      for (const decl::StructField &field: strukt->fields) {
        fields.push_back(
            CheckedStructField{field.name.id_value, field.type, offset});
        offset += 8;
      }
      return CheckedStruct{name, fields, offset};
    }
  }

  if (_registry != nullptr) {
    for (const auto &[module_name, module]: _registry->modules()) {
      auto strukt = _registry->find_struct(module_name, name);
      if (strukt.has_value())
        return strukt;
    }
  }

  return std::nullopt;
}

size_t Generator::struct_size(Type *type) {
  if (type->type != TYPE_STRUCT)
    return 8;
  std::string_view name = std::get<type::Struct *>(type->data)->name;
  auto strukt = find_struct(name);
  if (!strukt.has_value())
    return 8;
  return strukt->size;
}

void Generator::store_struct_fields(Function *fn, Operand base, Expr *value) {
  if (value->type == EXPR_STRUCT_LIT) {
    auto *lit = std::get<expr::StructLit *>(value->data);
    auto strukt = find_struct(lit->type_name.id_value);
    if (!strukt.has_value())
      PANIC("struct not found during codegen");

    for (const expr::StructLitField &field: lit->fields) {
      size_t offset = 0;
      for (const auto &struct_field: strukt->fields) {
        if (struct_field.name == field.name.id_value) {
          offset = struct_field.offset;
          break;
        }
      }
      Operand val = gen_expr(field.value, fn);
      fn->store_offset(base, static_cast<int64_t>(offset), val);
    }
    return;
  }

  if (value->type == EXPR_STRING) {
    auto *str = std::get<expr::String *>(value->data);
    std::string decoded = unescape_string(str->value.string_value);
    std::string label = _builder.create_rodata(decoded + '\0');

    Operand ptr = _builder.new_temp();
    fn->load_label(ptr, Operand::Label(label));
    fn->store_offset(base, 0, ptr);
    fn->store_offset(base, 8, Operand::ConstantInt(
                                    static_cast<int64_t>(decoded.size())));
    return;
  }

  PANIC("expected struct literal or string for struct initialization");
}

void Generator::init_struct_var(Function *fn, const std::string &name,
                                Type *type, Expr *value) {
  Operand base = Operand::Variable(name);
  store_struct_fields(fn, base, value);
}
