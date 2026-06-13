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
  case TYPE_BYTE: out << "\033[36mbyte\033[0m"; break;
  case TYPE_STRUCT:
    out << "\033[36m" << std::get<type::Struct *>(type->data)->name
        << "\033[0m";
    break;
  case TYPE_PROC: {
    auto *proc = std::get<type::Proc *>(type->data);
    out << "\033[36mproc(";
    for (size_t i = 0; i < proc->params.size(); i++) {
      if (i > 0)
        out << ", ";
      out << proc->params[i].name.id_value << " ";
      print_type(out, proc->params[i].type);
    }
    out << ") ";
    print_type(out, proc->ret_type);
    out << "\033[0m";
    break;
  }
  case TYPE_TUPLE: {
    auto *tuple = std::get<type::Tuple *>(type->data);
    out << "\033[36m(";
    for (size_t i = 0; i < tuple->elements.size(); i++) {
      if (i > 0)
        out << ", ";
      print_type(out, tuple->elements[i]);
    }
    out << ")\033[0m";
    break;
  }
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
    if (call->callee != nullptr) {
      out << "\n";
      print_expr(out, call->callee, indent.next(false));
    } else if (call->name.has_value()) {
      out << "\033[32m" << call->name->id_value << "\033[0m\n";
    }

    for (size_t i = 0; i < call->arguments.size(); ++i)
      print_expr(out, call->arguments[i],
                 indent.next(i == call->arguments.size() - 1));
    break;
  }

  case EXPR_COMPTIME_CALL: {
    auto call = std::get<expr::ComptimeCall *>(expr->data);
    out << "comptime \033[32m" << call->name.id_value << "\033[0m\n";
    for (size_t i = 0; i < call->arguments.size(); ++i)
      print_expr(out, call->arguments[i],
                 indent.next(i == call->arguments.size() - 1));
    break;
  }

  case EXPR_NOT:
    out << "\n";
    print_expr(out, std::get<expr::Not *>(expr->data)->expr, indent.next(true));
    break;

  case EXPR_FIELD: {
    auto field = std::get<expr::Field *>(expr->data);
    out << ".\033[34m" << field->field.id_value << "\033[0m\n";
    print_expr(out, field->base, indent.next(true));
    break;
  }

  case EXPR_STRUCT_LIT: {
    auto lit = std::get<expr::StructLit *>(expr->data);
    out << " \033[36m" << lit->type_name.id_value << "\033[0m\n";
    for (size_t i = 0; i < lit->fields.size(); ++i) {
      out << indent.next(i == lit->fields.size() - 1).prefix();
      out << "\033[34m" << lit->fields[i].name.id_value << "\033[0m\n";
      print_expr(out, lit->fields[i].value, indent.next(true));
    }
    break;
  }

  case EXPR_STRING: {
    auto str = std::get<expr::String *>(expr->data);
    out << " \033[33m\"" << str->value.string_value << "\"\033[0m\n";
    break;
  }

  case EXPR_SIZEOF:
    out << " sizeof(...)\n";
    break;

  case EXPR_CAST: {
    auto cast_ = std::get<expr::Cast *>(expr->data);
    out << " cast\n";
    print_expr(out, cast_->expr, indent.next(true));
    break;
  }

  case EXPR_INDEX: {
    auto index = std::get<expr::Index *>(expr->data);
    out << " []\n";
    print_expr(out, index->base, indent.next(false));
    print_expr(out, index->index, indent.next(true));
    break;
  }

  case EXPR_PROC_LIT:
    out << " proc { ... }\n";
    break;

  case EXPR_TUPLE: {
    auto *tuple = std::get<expr::TupleLit *>(expr->data);
    out << " tuple\n";
    for (size_t i = 0; i < tuple->elements.size(); i++) {
      print_expr(out, tuple->elements[i], indent.next(i == tuple->elements.size() - 1));
    }
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
    if (var->type != nullptr)
      print_type(out, var->type);
    if (var->value.has_value()) {
      out << "\n";
      print_expr(out, var->value.value(), indent.next(true));
    }
  } break;
  case STMT_RETURN: {
    auto ret = std::get<stmt::Return *>(stmt->data);
    out << "\n";
    if (ret->value.has_value())
      print_expr(out, *ret->value, indent.next(true));
  } break;
  case STMT_ASSIGN: {
    auto assign = std::get<stmt::Assign *>(stmt->data);
    out << "\n";
    print_expr(out, assign->target, indent.next(false));
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
  case STMT_WHEN: {
    auto when = std::get<stmt::When *>(stmt->data);
    out << "\n";
    print_expr(out, when->cond, indent.next(false));
    for (size_t i = 0; i < when->true_block.size(); ++i)
      print_stmt(out, when->true_block[i],
                 indent.next(i == when->true_block.size() - 1));
    if (!when->false_block.empty()) {
      out << indent.prefix() << "else\n";
      for (size_t i = 0; i < when->false_block.size(); ++i)
        print_stmt(out, when->false_block[i],
                   indent.next(i == when->false_block.size() - 1));
    }
  } break;
  case STMT_BREAK:
  case STMT_CONTINUE:
    out << "\n";
    break;
  case STMT_EXPR: {
    auto *expr_stmt = std::get<stmt::ExprStmt *>(stmt->data);
    out << "\n";
    print_expr(out, expr_stmt->expr, indent.next(true));
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
    if (proc->is_comptime)
      out << " \033[31mcomptime";
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

  case DECL_IMPORT: {
    auto import = std::get<decl::Import *>(decl->data);
    out << " \033[33m\"" << import->path.string_value << "\"\033[0m\n";
    break;
  }

  case DECL_COMPTIME_BLOCK: {
    auto block = std::get<decl::ComptimeBlock *>(decl->data);
    out << " \033[31mcomptime\033[0m\n";
    for (size_t i = 0; i < block->decls.size(); ++i)
      print_decl(out, block->decls[i],
                 indent.next(i == block->decls.size() - 1));
    break;
  }

  case DECL_STRUCT: {
    auto strukt = std::get<decl::Struct *>(decl->data);
    out << " \033[36m" << strukt->name.id_value << "\033[0m\n";
    for (size_t i = 0; i < strukt->fields.size(); ++i) {
      out << indent.next(i == strukt->fields.size() - 1).prefix();
      out << "\033[34m" << strukt->fields[i].name.id_value << "\033[0m ";
      print_type(out, strukt->fields[i].type);
      out << "\n";
    }
    break;
  }
  }
}
