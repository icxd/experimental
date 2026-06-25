#include <algorithm>
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
  for (const auto &lambda: _lambdas)
    gen_lambda(lambda);
}

void Generator::gen_lambda(const CheckedLambda &lambda) {
  std::vector<std::string_view> params{};
  for (const auto &param: lambda.params)
    params.push_back(param.name);

  std::string_view fn_name = own_name(
      link_name(_module_name, lambda.codegen_name, LINK_INTERN));
  Function *fn = _builder.create_function(fn_name, params);
  fn->linkage = LINK_INTERN;
  for (const auto &param: lambda.params) {
    std::string pname(param.name);
    if (param.type->type == TYPE_PTR) {
      fn->variable_sizes[pname] = 8;
      fn->indirect_vars.insert(pname);
    } else if (param.type->type == TYPE_STRUCT) {
      fn->variable_sizes[pname] = 8;
      fn->indirect_vars.insert(pname);
    } else {
      fn->variable_sizes[pname] = 8;
    }
  }
  for (const auto &stmt: lambda.body)
    gen_stmt(stmt, fn);

  optimize_function(*fn);
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

  std::string_view symbol = proc->codegen_name.empty()
                                 ? proc->name.id_value
                                 : proc->codegen_name;
  std::string_view fn_name = own_name(
        link_name(_module_name, symbol, proc->linkage));
    Function *fn = _builder.create_function(fn_name, params);
    fn->linkage = proc->linkage;
    for (const auto &param: proc->params) {
      std::string pname(param.name.id_value);
      if (param.type->type == TYPE_PTR) {
        fn->variable_sizes[pname] = 8;
        fn->indirect_vars.insert(pname);
      } else if (param.type->type == TYPE_STRUCT) {
        fn->variable_sizes[pname] = 8;
        fn->indirect_vars.insert(pname);
      } else {
        fn->variable_sizes[pname] = 8;
      }
    }
    for (const auto &stmt: proc->body)
      gen_stmt(stmt, fn);

    optimize_function(*fn);
  } break;

  case DECL_IMPORT: break;

  case DECL_STRUCT: break;

  case DECL_ENUM: break;

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
    if (var->type->type == TYPE_STRUCT || var->type->type == TYPE_TUPLE) {
      fn->variable_sizes[name] = aggregate_size(var->type);
      if (var->value.has_value()) {
        Expr *init = var->value.value();
        if (init->type == EXPR_TUPLE || init->type == EXPR_STRUCT_LIT ||
            init->type == EXPR_STRING) {
          init_aggregate_var(fn, name, var->type, init);
        } else {
          Operand src = gen_expr(init, fn);
          copy_aggregate(fn, Operand::Variable(name), src,
                         aggregate_size(var->type));
        }
      }
    } else {
      fn->variable_sizes[name] = 8;
      if (var->type->type == TYPE_PTR)
        fn->indirect_vars.insert(name);
      if (var->value.has_value()) {
        Operand src = gen_expr(var->value.value(), fn);
        fn->assign(Operand::Variable(name), src);
      }
    }
  } break;

  case STMT_ASSIGN: {
    auto assign = std::get<stmt::Assign *>(stmt->data);
    Operand src = gen_expr(assign->value, fn);
    store_lvalue(fn, assign->target, src);
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

  case STMT_EXPR: {
    auto *expr_stmt = std::get<stmt::ExprStmt *>(stmt->data);
    gen_expr(expr_stmt->expr, fn);
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
    if (ref->expr->type == EXPR_FIELD) {
      auto *field = std::get<expr::Field *>(ref->expr->data);
      size_t offset = field_offset(field);
      Type *base_type = field->base->expr_type;
      if (base_type != nullptr && base_type->type == TYPE_PTR) {
        Operand base = gen_field_load_base(field->base, fn);
        Operand ptr_val = _builder.new_temp();
        fn->assign(ptr_val, base);
        if (offset == 0)
          return ptr_val;
        Operand off_addr = _builder.new_temp();
        fn->add(off_addr, ptr_val,
                Operand::ConstantInt(static_cast<int64_t>(offset)));
        return off_addr;
      }
      Operand base = gen_field_load_base(field->base, fn);
      Operand addr = _builder.new_temp();
      if (base.type == OPERAND_VARIABLE)
        fn->addrof(addr, base);
      else
        fn->assign(addr, base);
      if (offset == 0)
        return addr;
      Operand off_addr = _builder.new_temp();
      fn->add(off_addr, addr, Operand::ConstantInt(static_cast<int64_t>(offset)));
      return off_addr;
    }
    if (ref->expr->type == EXPR_VAR && expr->expr_type != nullptr &&
        expr->expr_type->type == TYPE_PROC) {
      auto *var = std::get<expr::Var *>(ref->expr->data);
      std::string_view module = _module_name;
      if (var->module.has_value())
        module = var->module->id_value;

      std::optional<CheckedProc> proc = find_proc(module, var->var.id_value);
      if (!proc.has_value()) {
        for (const auto &import_module: _imports) {
          proc = find_proc(import_module, var->var.id_value);
          if (proc.has_value()) {
            module = import_module;
            break;
          }
        }
      }

      if (proc.has_value()) {
        Operand dst = _builder.new_temp();
        Linkage linkage = find_proc_linkage(module, var->var.id_value);
        std::string callee = link_name(module, var->var.id_value, linkage);
        fn->load_label(dst, Operand::Function(callee));
        return dst;
      }
    }

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

  case EXPR_CAST: {
    auto cast_ = std::get<expr::Cast *>(expr->data);
    Type *target = expr->expr_type;
    Type *from = cast_->expr->expr_type;
    if (from != nullptr && from->type == TYPE_UNION && target != nullptr &&
        target->type == TYPE_STRUCT) {
      Operand src = gen_expr(cast_->expr, fn);
      if (src.type != OPERAND_VARIABLE)
        PANIC("union member cast requires variable operand");
      std::string temp = std::format("__cast_tmp_{}", _struct_tmp_counter++);
      size_t copy_size = type_size(target);
      fn->variable_sizes[temp] = copy_size;
      Operand dst = Operand::Variable(temp);
      copy_aggregate(fn, dst, src, copy_size);
      return dst;
    }
    return gen_expr(cast_->expr, fn);
  }

  case EXPR_SIZEOF: {
    auto *sz = std::get<expr::Sizeof *>(expr->data);
    return Operand::ConstantInt(static_cast<int64_t>(type_size(sz->type)));
  }

  case EXPR_INDEX: {
    auto index = std::get<expr::Index *>(expr->data);
    if (index->base->expr_type->type == TYPE_TUPLE) {
      Operand addr = gen_tuple_element_addr(index, fn);
      Operand dst = _builder.new_temp();
      fn->deref(dst, addr);
      return dst;
    }
    Operand addr = gen_index_addr(index, fn);
    Operand dst = _builder.new_temp();
    Type *elem_type = std::get<type::Ptr *>(index->base->expr_type->data)->inner;
    if (elem_type->type == TYPE_BYTE) {
      fn->load_byte(dst, addr);
    } else {
      fn->deref(dst, addr);
    }
    return dst;
  }

  case EXPR_TUPLE: {
    std::string temp = std::format("__tuple_tmp_{}", _struct_tmp_counter++);
    Type *type = expr->expr_type;
    fn->variable_sizes[temp] = tuple_size(type);
    init_aggregate_var(fn, temp, type, expr);
    return Operand::Variable(temp);
  }

  case EXPR_COMPTIME_CALL:
    PANIC("unreachable");

  case EXPR_PROC_LIT: {
    auto *lit = std::get<expr::ProcLit *>(expr->data);
    Operand dst = _builder.new_temp();
    std::string fn_name =
        link_name(_module_name, lit->codegen_name, LINK_INTERN);
    fn->load_label(dst, Operand::Function(fn_name));
    return dst;
  }

  case EXPR_CALL: {
    auto call = std::get<expr::Call *>(expr->data);

    if (call->callee != nullptr) {
      Operand callee = gen_expr(call->callee, fn);
      Type *callee_type = call->callee->expr_type;
      if (callee_type->type != TYPE_PROC)
        PANIC("indirect call requires procedure callee");
      auto *sig = std::get<type::Proc *>(callee_type->data);

      Operand dst = _builder.new_temp();
      std::vector<Operand> args{};
      for (size_t i = 0; i < call->arguments.size(); i++)
        args.push_back(
            gen_call_arg(call->arguments[i], fn, sig->params[i].type));
      fn->call(dst, callee, args);
      return dst;
    }

    std::string_view module = _module_name;
    if (call->module.has_value())
      module = call->module->id_value;
    else if (call->resolved_module.has_value())
      module = *call->resolved_module;
    std::string_view callee_symbol = call->name->id_value;
    if (call->resolved_codegen_name.has_value())
      callee_symbol = *call->resolved_codegen_name;
    Linkage linkage = find_proc_linkage(module, callee_symbol);
    std::string callee_name = link_name(module, callee_symbol, linkage);
    Operand name = Operand::Function(callee_name);

    auto proc = find_proc(module, callee_symbol);
    if (!proc.has_value())
      PANIC("procedure not found during codegen");

    Operand dst;
    if (proc->ret_type->type == TYPE_TUPLE) {
      std::string temp = std::format("__call_ret_{}", _struct_tmp_counter++);
      fn->variable_sizes[temp] = tuple_size(proc->ret_type);
      dst = Operand::Variable(temp);
    } else if (proc->ret_type->type == TYPE_STRUCT) {
      std::string temp = std::format("__call_ret_{}", _struct_tmp_counter++);
      fn->variable_sizes[temp] = struct_size(proc->ret_type);
      dst = Operand::Variable(temp);
    } else {
      dst = _builder.new_temp();
    }

    std::vector<Operand> args{};
    size_t param_index = 0;
    if (call->receiver != nullptr) {
      args.push_back(gen_call_arg(call->receiver, fn,
                                  proc->params[param_index++].type));
    }
    for (auto *arg: call->arguments) {
      args.push_back(gen_call_arg(arg, fn, proc->params[param_index++].type));
    }
    fn->call(dst, name, args);
    return dst;
  }

  case EXPR_FIELD: {
    auto field = std::get<expr::Field *>(expr->data);
    if (field->base->type == EXPR_VAR) {
      auto *var = std::get<expr::Var *>(field->base->data);
      if (!var->module.has_value()) {
        if (auto value =
                enum_member_value(var->var.id_value, field->field.id_value);
            value.has_value())
          return Operand::ConstantInt(*value);
      }
    }
    size_t offset = field_offset(field);
    Operand base = gen_field_load_base(field->base, fn);
    Type *field_type = expr->expr_type;
    if (field_type->type == TYPE_STRUCT || field_type->type == TYPE_TUPLE ||
        field_type->type == TYPE_UNION) {
      std::string temp = std::format("__field_tmp_{}", _struct_tmp_counter++);
      size_t size = field_type->type == TYPE_UNION
                          ? union_size(field_type)
                          : aggregate_size(field_type);
      fn->variable_sizes[temp] = size;
      Operand dst = Operand::Variable(temp);
      copy_aggregate_from_offset(fn, dst, base, static_cast<int64_t>(offset),
                                 size);
      return dst;
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

  case EXPR_ENUM_CASE: {
    auto *case_ = std::get<expr::EnumCase *>(expr->data);
    if (expr->expr_type == nullptr || expr->expr_type->type != TYPE_ENUM)
      PANIC("enum case missing type");
    std::string_view enum_name =
        std::get<type::Enum *>(expr->expr_type->data)->name;
    if (auto value = enum_member_value(enum_name, case_->member.id_value);
        value.has_value())
      return Operand::ConstantInt(*value);
    PANIC("unknown enum case during codegen");
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
      std::string_view symbol = proc->codegen_name.empty()
                                    ? proc->name.id_value
                                    : proc->codegen_name;
      if (symbol == name)
        return proc->linkage;
    }
  }

  if (_registry != nullptr) {
    auto proc = _registry->find_proc(module, name);
    if (proc.has_value())
      return proc->linkage;
  }

  return LINK_INTERN;
}

size_t Generator::field_offset(expr::Field *field) {
  Type *base_type = field->base->expr_type;
  Type *struct_type = base_type->as_struct_for_field_access();
  if (struct_type == nullptr)
    PANIC("field access requires struct or pointer to struct");
  std::string_view struct_name =
      std::get<type::Struct *>(struct_type->data)->name;
  auto strukt = find_struct(struct_name);
  if (!strukt.has_value())
    PANIC("struct not found during codegen");
  for (const auto &struct_field: strukt->fields) {
    if (struct_field.name == field->field.id_value)
      return struct_field.offset;
  }
  PANIC("field not found during codegen");
}

Operand Generator::gen_field_load_base(Expr *base_expr, Function *fn) {
  if (base_expr->type == EXPR_GROUP)
    base_expr = std::get<expr::Group *>(base_expr->data)->expr;

  switch (base_expr->type) {
  case EXPR_VAR:
    return Operand::Variable(std::string(
        std::get<expr::Var *>(base_expr->data)->var.id_value));
  case EXPR_DEREF: {
    auto *deref = std::get<expr::Deref *>(base_expr->data);
    Operand ptr = gen_expr(deref->expr, fn);
    Operand ptr_val = _builder.new_temp();
    fn->assign(ptr_val, ptr);
    return ptr_val;
  }
  case EXPR_FIELD: {
    Operand inner = gen_expr(base_expr, fn);
    Operand inner_val = _builder.new_temp();
    fn->assign(inner_val, inner);
    return inner_val;
  }
  case EXPR_CAST: {
    auto *cast_ = std::get<expr::Cast *>(base_expr->data);
    Operand ptr = gen_expr(cast_->expr, fn);
    Operand ptr_val = _builder.new_temp();
    fn->assign(ptr_val, ptr);
    return ptr_val;
  }
  default:
    PANIC("field access only supported on variables and pointers");
  }
}

void Generator::store_lvalue(Function *fn, Expr *target, Operand src) {
  switch (target->type) {
  case EXPR_VAR: {
    auto *var = std::get<expr::Var *>(target->data);
    fn->assign(Operand::Variable(std::string(var->var.id_value)), src);
    return;
  }

  case EXPR_FIELD: {
    auto *field = std::get<expr::Field *>(target->data);
    size_t offset = field_offset(field);
    Operand base = gen_field_load_base(field->base, fn);
    fn->store_offset(base, static_cast<int64_t>(offset), src);
    return;
  }

  case EXPR_DEREF: {
    auto *deref = std::get<expr::Deref *>(target->data);
    Operand ptr = gen_expr(deref->expr, fn);
    Operand ptr_val = _builder.new_temp();
    fn->assign(ptr_val, ptr);
    fn->store_offset(ptr_val, 0, src);
    return;
  }

  case EXPR_INDEX: {
    auto *index = std::get<expr::Index *>(target->data);
    if (index->base->expr_type->type == TYPE_TUPLE) {
      Operand addr = gen_tuple_element_addr(index, fn);
      fn->store_offset(addr, 0, src);
      return;
    }
    Operand addr = gen_index_addr(index, fn);
    Type *elem_type =
        std::get<type::Ptr *>(index->base->expr_type->data)->inner;
    if (elem_type->type == TYPE_BYTE) {
      fn->store_byte(addr, src);
    } else {
      fn->store_offset(addr, 0, src);
    }
    return;
  }

  default:
    PANIC("invalid assignment target during codegen");
  }
}

std::optional<CheckedProc> Generator::find_proc(std::string_view module,
                                                std::string_view name) {
  if (module == _module_name) {
    for (Decl *decl: _decls) {
      if (decl->type != DECL_PROC)
        continue;
      auto *proc = std::get<decl::Proc *>(decl->data);
      if (proc->is_comptime)
        continue;
      if (proc->name.id_value == name ||
          (!proc->codegen_name.empty() && proc->codegen_name == name)) {
        std::vector<CheckedParam> params{};
        for (const Param &param: proc->params)
          params.push_back({param.name.id_value, param.type});
        Type *ret_type = proc->ret_type.value_or(
            new Type{TYPE_VOID, proc->name.start, proc->name.end});
        return CheckedProc{
            .name = name,
            .codegen_name = proc->codegen_name.empty()
                                ? std::string(proc->name.id_value)
                                : proc->codegen_name,
            .params = params,
            .ret_type = ret_type,
            .scope = nullptr,
            .linkage = proc->linkage,
            .decl = proc,
        };
      }
    }
  }

  if (_registry != nullptr)
    return _registry->find_proc(module, name);
  return std::nullopt;
}

Operand Generator::gen_call_arg(Expr *arg, Function *fn, Type *param_type) {
  Type *arg_type = arg->expr_type;

  if (param_type->type == TYPE_STRUCT && arg_type->type == TYPE_STRUCT) {
    if (arg->type == EXPR_VAR) {
      auto *var = std::get<expr::Var *>(arg->data);
      return Operand::StackAddr(std::string(var->var.id_value));
    }
    if (arg->type == EXPR_STRUCT_LIT || arg->type == EXPR_STRING) {
      Operand value = gen_expr(arg, fn);
      if (value.type == OPERAND_VARIABLE)
        return Operand::StackAddr(value.name);
    }
    Operand value = gen_expr(arg, fn);
    if (value.type == OPERAND_VARIABLE)
      return Operand::StackAddr(value.name);
  }

  if (param_type->type == TYPE_TUPLE && arg_type->type == TYPE_TUPLE) {
    if (arg->type == EXPR_VAR) {
      auto *var = std::get<expr::Var *>(arg->data);
      return Operand::StackAddr(std::string(var->var.id_value));
    }
    Operand value = gen_expr(arg, fn);
    if (value.type == OPERAND_VARIABLE)
      return Operand::StackAddr(value.name);
  }

  if (param_type->type == TYPE_PTR && arg_type->type == TYPE_STRUCT &&
      arg->is_lvalue()) {
    Operand dst = _builder.new_temp();
    if (arg->type == EXPR_VAR) {
      auto *var = std::get<expr::Var *>(arg->data);
      fn->addrof(dst, Operand::Variable(std::string(var->var.id_value)));
      return dst;
    }
  }

  return gen_expr(arg, fn);
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
        offset += type_size(field.type);
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

std::optional<CheckedEnum> Generator::find_enum(std::string_view name) {
  for (Decl *decl: _decls) {
    if (decl->type != DECL_ENUM)
      continue;
    auto *enum_ = std::get<decl::Enum *>(decl->data);
    if (enum_->name.id_value != name)
      continue;
    CheckedEnum checked{.name = name, .underlying_type = nullptr};
    int64_t next_value = 0;
    for (const decl::EnumMember &member: enum_->members) {
      if (member.value.has_value() &&
          member.value.value()->type == EXPR_INT)
        next_value = std::get<expr::Int *>(member.value.value()->data)->value;
      checked.members.push_back(
          CheckedEnumMember{member.name.id_value, next_value});
      next_value++;
    }
    checked.members.push_back(
        CheckedEnumMember{"count",
                          static_cast<int64_t>(enum_->members.size())});
    return checked;
  }

  if (_registry != nullptr) {
    for (const auto &[module_name, module]: _registry->modules()) {
      auto enum_ = _registry->find_enum(module_name, name);
      if (enum_.has_value())
        return enum_;
    }
  }

  return std::nullopt;
}

std::optional<int64_t> Generator::enum_member_value(std::string_view enum_name,
                                                    std::string_view member) {
  auto enum_ = find_enum(enum_name);
  if (!enum_.has_value())
    return std::nullopt;
  for (const auto &enum_member: enum_->members) {
    if (enum_member.name == member)
      return enum_member.value;
  }
  return std::nullopt;
}

size_t Generator::tuple_size(Type *type) {
  if (type->type != TYPE_TUPLE)
    PANIC("tuple_size requires tuple type");
  size_t size = 0;
  for (Type *element: std::get<type::Tuple *>(type->data)->elements)
    size += type_size(element);
  return size;
}

size_t Generator::aggregate_size(Type *type) {
  if (type->type == TYPE_TUPLE)
    return tuple_size(type);
  if (type->type == TYPE_STRUCT)
    return struct_size(type);
  PANIC("aggregate_size requires struct or tuple type");
}

size_t Generator::tuple_element_offset(Type *tuple_type, size_t index) {
  size_t offset = 0;
  auto *tuple = std::get<type::Tuple *>(tuple_type->data);
  for (size_t i = 0; i < index; i++)
    offset += type_size(tuple->elements[i]);
  return offset;
}

size_t Generator::union_size(Type *type) {
  if (type->type != TYPE_UNION)
    PANIC("union_size requires union type");
  size_t max_size = 0;
  for (Type *member: std::get<type::Union *>(type->data)->members)
    max_size = std::max(max_size, type_size(member));
  return max_size;
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

size_t Generator::type_size(Type *type) {
  switch (type->type) {
  case TYPE_VOID: return 0;
  case TYPE_BOOL:
  case TYPE_INT:
  case TYPE_BYTE:
  case TYPE_PTR:  return 8;
  case TYPE_STRUCT: return struct_size(type);
  case TYPE_ENUM: return 8;
  case TYPE_UNION: return union_size(type);
  case TYPE_TUPLE: return tuple_size(type);
  case TYPE_PROC: return 8;
  }
  std::unreachable();
}

size_t Generator::index_element_size(Type *type) {
  if (type->type == TYPE_BYTE || type->type == TYPE_BOOL)
    return 1;
  return type_size(type);
}

Operand Generator::gen_tuple_element_addr(expr::Index *index, Function *fn) {
  auto *idx_lit = std::get<expr::Int *>(index->index->data);
  size_t elem_index = static_cast<size_t>(idx_lit->value);
  Type *tuple_type = index->base->expr_type;
  size_t offset = tuple_element_offset(tuple_type, elem_index);

  Expr *base_expr = index->base;
  if (base_expr->type == EXPR_GROUP)
    base_expr = std::get<expr::Group *>(base_expr->data)->expr;

  if (base_expr->type != EXPR_VAR)
    PANIC("tuple index base must be variable");

  Operand base = Operand::Variable(
      std::string(std::get<expr::Var *>(base_expr->data)->var.id_value));
  Operand addr = _builder.new_temp();
  fn->addrof(addr, base);
  if (offset == 0)
    return addr;
  Operand off_addr = _builder.new_temp();
  fn->add(off_addr, addr, Operand::ConstantInt(static_cast<int64_t>(offset)));
  return off_addr;
}

Operand Generator::gen_index_addr(expr::Index *index, Function *fn) {
  Type *base_type = index->base->expr_type;
  Type *elem_type = std::get<type::Ptr *>(base_type->data)->inner;
  size_t elem_size = index_element_size(elem_type);

  Operand base = gen_expr(index->base, fn);
  Operand idx = gen_expr(index->index, fn);

  if (elem_size == 1) {
    Operand addr = _builder.new_temp();
    fn->add(addr, base, idx);
    return addr;
  }

  Operand offset = _builder.new_temp();
  fn->mul(offset, idx, Operand::ConstantInt(static_cast<int64_t>(elem_size)));
  Operand addr = _builder.new_temp();
  fn->add(addr, base, offset);
  return addr;
}

void Generator::copy_aggregate(Function *fn, Operand dst, Operand src,
                               size_t size) {
  if (src.type != OPERAND_VARIABLE || dst.type != OPERAND_VARIABLE)
    PANIC("aggregate copy requires variable operands");
  copy_aggregate_to_offset(fn, dst, 0, src, size);
}

void Generator::copy_aggregate_to_offset(Function *fn, Operand dst_base,
                                         int64_t dst_offset, Operand src,
                                         size_t size) {
  if (src.type != OPERAND_VARIABLE)
    PANIC("aggregate copy requires variable source");
  for (size_t off = 0; off < size; off += 8) {
    Operand chunk = _builder.new_temp();
    fn->load_offset(chunk, src, static_cast<int64_t>(off));
    fn->store_offset(dst_base, dst_offset + static_cast<int64_t>(off), chunk);
  }
}

void Generator::copy_aggregate_from_offset(Function *fn, Operand dst,
                                         Operand src_base, int64_t src_offset,
                                         size_t size) {
  if (dst.type != OPERAND_VARIABLE)
    PANIC("aggregate copy requires variable destination");
  for (size_t off = 0; off < size; off += 8) {
    Operand chunk = _builder.new_temp();
    fn->load_offset(chunk, src_base, src_offset + static_cast<int64_t>(off));
    fn->store_offset(dst, static_cast<int64_t>(off), chunk);
  }
}

void Generator::init_aggregate_var(Function *fn, const std::string &name,
                                   Type *type, Expr *value) {
  Operand base = Operand::Variable(name);
  if (value->type == EXPR_TUPLE) {
    auto *lit = std::get<expr::TupleLit *>(value->data);
    size_t offset = 0;
    for (Expr *element: lit->elements) {
      Operand val = gen_expr(element, fn);
      fn->store_offset(base, static_cast<int64_t>(offset), val);
      offset += type_size(element->expr_type);
    }
    return;
  }
  store_struct_fields(fn, base, value);
}

void Generator::init_struct_var(Function *fn, const std::string &name,
                                Type *type, Expr *value) {
  init_aggregate_var(fn, name, type, value);
}

void Generator::store_struct_fields(Function *fn, Operand base, Expr *value) {
  if (value->type == EXPR_STRUCT_LIT) {
    auto *lit = std::get<expr::StructLit *>(value->data);
    auto strukt = find_struct(lit->type_name.id_value);
    if (!strukt.has_value())
      PANIC("struct not found during codegen");

    for (const expr::StructLitField &field: lit->fields) {
      size_t offset = 0;
      Type *field_type = nullptr;
      for (const auto &struct_field: strukt->fields) {
        if (struct_field.name == field.name.id_value) {
          offset = struct_field.offset;
          field_type = struct_field.type;
          break;
        }
      }
      Operand val = gen_expr(field.value, fn);
      if (field_type != nullptr && field_type->type == TYPE_UNION) {
        if (val.type != OPERAND_VARIABLE)
          PANIC("union field init requires variable operand");
        size_t copy_size = type_size(field.value->expr_type);
        copy_aggregate_to_offset(fn, base, static_cast<int64_t>(offset), val,
                                 copy_size);
      } else if (field_type != nullptr &&
          (field_type->type == TYPE_STRUCT || field_type->type == TYPE_TUPLE)) {
        if (val.type != OPERAND_VARIABLE)
          PANIC("aggregate struct field init requires variable operand");
        copy_aggregate_to_offset(fn, base, static_cast<int64_t>(offset), val,
                                 type_size(field_type));
      } else {
        fn->store_offset(base, static_cast<int64_t>(offset), val);
      }
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
