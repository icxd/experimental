#pragma once

#include <common.hpp>
#include <magic_enum/magic_enum.hpp>
#include <parser/ast.hpp>

struct Indent {
  std::vector<bool> levels; // true = last, false = has sibling

  std::string prefix() const {
    std::string result = "\033[31m";
    for (size_t i = 0; i < levels.size(); ++i) {
      if (i + 1 == levels.size())
        result += levels[i] ? "└─" : "├─";
      else
        result += levels[i] ? "  " : "│ ";
    }
    result += "\033[0m";
    return result;
  }

  Indent next(bool is_last) const {
    Indent copy = *this;
    copy.levels.push_back(is_last);
    return copy;
  }
};

inline void print_type(std::ostream &out, Type *type) {
  switch (type->type) {
  case TYPE_VOID: out << "\033[36mvoid\033[0m"; break;
  case TYPE_BOOL: out << "\033[36mbool\033[0m"; break;
  case TYPE_INT:  out << "\033[36mint\033[0m"; break;
  case TYPE_PTR:
    out << "\033[31m*";
    print_type(out, std::get<type::Ptr *>(type->data)->inner);
    out << "\033[0m";
    break;
  }
}

inline void print_expr(std::ostream &out, Expr *expr, Indent indent = {}) {
  out << indent.prefix();
  out << "\033[31m" << magic_enum::enum_name(expr->type) << " \033[34m"
      << (void *) expr << " \033[36m<" << expr->start << "> \033[31m(";
  if (expr->expr_type)
    print_type(out, expr->expr_type);
  else
    out << "<null>";
  out << "\033[31m) \033[0m";

  switch (expr->type) {
  case EXPR_INT:
    out << "\033[33m" << std::get<expr::Int *>(expr->data)->value
        << "\033[0m\n";
    break;

  case EXPR_BOOL:
    out << "\033[33m"
        << (std::get<expr::Bool *>(expr->data)->value ? "true" : "false")
        << "\033[0m\n";
    break;

  case EXPR_VAR:
    out << "\033[34m" << std::get<expr::Var *>(expr->data)->var.id_value
        << "\033[0m\n";
    break;

  case EXPR_GROUP:
    out << "\n";
    print_expr(out, std::get<expr::Group *>(expr->data)->expr,
               indent.next(true));
    break;

  case EXPR_BINARY: {
    auto bin = std::get<expr::Binary *>(expr->data);
    out << " " << binop_to_string(bin->op) << "\n";
    print_expr(out, bin->lhs, indent.next(false));
    print_expr(out, bin->rhs, indent.next(true));
  } break;

  case EXPR_REF:
    out << "\n";
    print_expr(out, std::get<expr::Ref *>(expr->data)->expr, indent.next(true));
    break;

  case EXPR_DEREF:
    out << "\n";
    print_expr(out, std::get<expr::Deref *>(expr->data)->expr,
               indent.next(true));
    break;

  case EXPR_CALL: {
    auto call = std::get<expr::Call *>(expr->data);
    out << "\033[32m" << call->name.id_value << "\033[0m\n";

    for (size_t i = 0; i < call->arguments.size(); ++i)
      print_expr(out, call->arguments[i],
                 indent.next(i == call->arguments.size() - 1));
    break;
  }
  }
}

inline void print_stmt(std::ostream &out, Stmt *stmt, Indent indent = {}) {
  out << indent.prefix();
  out << "\033[31m" << magic_enum::enum_name(stmt->type) << " \033[34m"
      << (void *) stmt << " \033[36m<" << stmt->start << ">\033[0m";

  switch (stmt->type) {
  case STMT_BLOCK: {
    auto block = std::get<stmt::Block *>(stmt->data);
    out << "\n";
  } break;
  case STMT_VAR: {
    auto var = std::get<stmt::Var *>(stmt->data);
    out << " \033[34m" << var->name.id_value << "\033[0m ";
    print_type(out, var->type);
    out << "\n";
    print_expr(out, var->value, indent.next(true));
  } break;
  case STMT_RETURN: {
    auto ret = std::get<stmt::Return *>(stmt->data);
    out << "\n";
    if (ret->value.has_value())
      print_expr(out, *ret->value, indent.next(true));
  } break;
  case STMT_ASSIGN: {
    auto assign = std::get<stmt::Assign *>(stmt->data);
    out << " \033[34m" << assign->name.id_value << "\033[0m\n";
    print_expr(out, assign->value, indent.next(true));
  } break;
  case STMT_WHILE: {
    auto while_ = std::get<stmt::While *>(stmt->data);
    out << "\n";
    print_expr(out, while_->cond, indent.next(false));
    for (size_t i = 0; i < while_->body.size(); ++i)
      print_stmt(out, while_->body[i],
                 indent.next(i == while_->body.size() - 1));
  } break;
  case STMT_FOR: {
    auto for_ = std::get<stmt::For *>(stmt->data);
    out << "\n";
    print_stmt(out, for_->init, indent.next(false));
    print_expr(out, for_->cond, indent.next(false));
    print_stmt(out, for_->step, indent.next(false));
    for (size_t i = 0; i < for_->body.size(); ++i)
      print_stmt(out, for_->body[i],
                 indent.next(i == for_->body.size() - 1));
  } break;
  case STMT_IF: {
    auto if_ = std::get<stmt::If *>(stmt->data);
    out << "\n";
    print_expr(out, if_->cond, indent.next(false));
    for (size_t i = 0; i < if_->then_block.size(); ++i)
      print_stmt(out, if_->then_block[i],
                 indent.next(i == if_->then_block.size() - 1));
    if (!if_->else_block.empty()) {
      out << indent.prefix() << "else\n";
      for (size_t i = 0; i < if_->else_block.size(); ++i)
        print_stmt(out, if_->else_block[i],
                   indent.next(i == if_->else_block.size() - 1));
    }
  } break;
  }
}

static inline void print_decl(std::ostream &out, Decl *decl,
                              Indent indent = {}) {
  out << indent.prefix();

  out << "\033[31m" << magic_enum::enum_name(decl->type) << " \033[34m"
      << (void *) decl << " \033[36m<" << decl->start << ">\033[0m";

  switch (decl->type) {
  case DECL_CONST: {
    auto const_ = std::get<decl::Const *>(decl->data);
    out << " \033[34m" << const_->name.id_value << " \033[31m";
    print_type(out, const_->type);
    out << "\n";
    print_expr(out, const_->value, indent.next(true));
    break;
  }

  case DECL_PROC: {
    auto proc = std::get<decl::Proc *>(decl->data);
    if (proc->linkage == LINK_EXTERN)
      out << " \033[31mextern";
    out << " \033[32m" << proc->name.id_value << " \033[31m(";
    for (size_t i = 0; i < proc->params.size(); i++) {
      auto param = proc->params[i];
      out << "\033[34m" << param.name.id_value << " \033[31m";
      print_type(out, param.type);
      if (i + 1 < proc->params.size())
        out << "\033[31m, \033[0m";
    }
    out << "\033[31m)\033[0m";
    if (proc->ret_type) {
      out << " ";
      print_type(out, *proc->ret_type);
    }

    out << "\n";

    for (size_t i = 0; i < proc->body.size(); ++i)
      print_stmt(out, proc->body[i], indent.next(i == proc->body.size() - 1));
    break;
  }

  case DECL_WHEN: {
    auto when = std::get<decl::When *>(decl->data);
    out << "\n";

    print_expr(out, when->cond, indent.next(false));

    for (size_t i = 0; i < when->true_block.size(); ++i)
      print_decl(out, when->true_block[i],
                 indent.next(i == when->true_block.size() - 1));

    if (when->false_block.size() > 0) {

      for (size_t i = 0; i < when->false_block.size(); ++i)
        print_decl(out, when->false_block[i],
                   indent.next(i == when->false_block.size() - 1));
    }
    break;
  }
  }
}
