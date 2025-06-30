#include <ir/ir.hpp>
#include <ir/irgen.hpp>
#include <vector>
#include "parser/ast.hpp"

void Generator::gen_decls() {
  for (auto *decl: _decls)
    gen_decl(decl);
}

void Generator::gen_decl(Decl *decl) {
  switch (decl->type) {
  case DECL_PROC: {
    auto proc = std::get<decl::Proc *>(decl->data);

    std::vector<std::string_view> params{};
    for (const auto &param: proc->params)
      params.push_back(param.name.id_value);

    Function *fn = _builder.create_function(proc->name.id_value, params);
    fn->linkage = proc->linkage;
    for (const auto &stmt: proc->body)
      gen_stmt(stmt, fn);

    fn->instructions = fold_constants(fn->instructions);
    fn->instructions = fold_temporaries(fn->instructions);
  } break;

  case DECL_WHEN: PANIC("unreachable");
  }
}

void Generator::gen_stmt(Stmt *stmt, Function *fn) {
  switch (stmt->type) {
  case STMT_BLOCK: {
    auto block = std::get<stmt::Block *>(stmt->data);
  } break;

  case STMT_VAR: {
    auto var = std::get<stmt::Var *>(stmt->data);
    Operand src = gen_expr(var->value, fn);
    fn->assign(Operand::Variable(std::string(var->name.id_value)), src);
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
    return Operand::ConstantInt(bool_->value);
  }

  case EXPR_VAR: {
    auto var = std::get<expr::Var *>(expr->data);
    return Operand::Variable(std::string(var->var.id_value));
  }

  case EXPR_GROUP: {
    auto group = std::get<expr::Group *>(expr->data);
    return gen_expr(group->expr, fn);
  }

  case EXPR_BINARY: {
    auto binary = std::get<expr::Binary *>(expr->data);
    Operand dst = _builder.new_temp();
    Operand lhs = gen_expr(binary->lhs, fn);
    Operand rhs = gen_expr(binary->rhs, fn);

    switch (binary->op) {
    case expr::Binary::BINOP_PLUS:  fn->add(dst, lhs, rhs); break;
    case expr::Binary::BINOP_MINUS: fn->sub(dst, lhs, rhs); break;
    case expr::Binary::BINOP_STAR:  fn->mul(dst, lhs, rhs); break;
    case expr::Binary::BINOP_SLASH: fn->div(dst, lhs, rhs); break;
    default:                        PANIC("todo");
    }

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

  case EXPR_CALL: {
    auto call = std::get<expr::Call *>(expr->data);
    Operand dst = _builder.new_temp();
    Operand name = Operand::Function(std::string(call->name.id_value));
    std::vector<Operand> args{};
    for (auto *arg: call->arguments)
      args.push_back(gen_expr(arg, fn));
    fn->call(dst, name, args);
    return dst;
  }
  }
}
