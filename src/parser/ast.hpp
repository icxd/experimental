#pragma once

#include <common.hpp>
#include <lexer/token.hpp>

struct Decl;
struct Stmt;
struct Expr;
struct Type;

using Block = std::vector<Stmt *>;

struct Param {
  Token name;
  Type *type;
};

enum Linkage { LINK_INTERN, LINK_EXTERN };

namespace decl {

  struct Proc {
    Token name;
    std::vector<Param> params;
    std::optional<Type *> ret_type;
    Linkage linkage;
    Block body;
  };

  struct When {
    Expr *cond;
    std::vector<Decl *> true_block;
    std::vector<Decl *> false_block;
  };

} // namespace decl

enum DeclType { DECL_PROC, DECL_WHEN };
struct Decl {
  DeclType type;
  size_t start, end;
  std::variant<decl::Proc *, decl::When *> data;
};

namespace stmt {

  struct Block {
    std::vector<Stmt *> stmts;
  };

  struct Var {
    Token name;
    Type *type;
    Expr *value;
  };

  struct Return {
    std::optional<Expr *> value;
  };

} // namespace stmt

enum StmtType { STMT_BLOCK, STMT_VAR, STMT_RETURN };
struct Stmt {
  StmtType type;
  size_t start, end;
  std::variant<stmt::Block *, stmt::Var *, stmt::Return *> data;
};

namespace expr {

  struct Int {
    int64_t value;
  };

  struct Bool {
    bool value;
  };

  struct Var {
    Token var;
  };

  struct Group {
    Expr *expr;
  };

  struct Binary {
    Expr *lhs;
    enum BinOp {
      BINOP_PLUS,
      BINOP_MINUS,
      BINOP_STAR,
      BINOP_SLASH,
      BINOP_EQ,
      BINOP_NEQ,
      BINOP_LT,
      BINOP_LTE,
      BINOP_GT,
      BINOP_GTE,
    } op;
    Expr *rhs;
  };

  struct Ref {
    Expr *expr;
  };

  struct Deref {
    Expr *expr;
  };

  struct Call {
    Token name;
    std::vector<Expr *> arguments;
  };

} // namespace expr

static std::string binop_to_string(expr::Binary::BinOp op) {
  switch (op) {
  case expr::Binary::BINOP_PLUS:  return "+";
  case expr::Binary::BINOP_MINUS: return "-";
  case expr::Binary::BINOP_STAR:  return "*";
  case expr::Binary::BINOP_SLASH: return "/";
  case expr::Binary::BINOP_EQ:    return "==";
  case expr::Binary::BINOP_NEQ:   return "!=";
  case expr::Binary::BINOP_LT:    return "<";
  case expr::Binary::BINOP_LTE:   return "<=";
  case expr::Binary::BINOP_GT:    return ">";
  case expr::Binary::BINOP_GTE:   return ">=";
  }
}

enum ExprType {
  EXPR_INT,
  EXPR_BOOL,
  EXPR_VAR,
  EXPR_GROUP,
  EXPR_BINARY,
  EXPR_REF,
  EXPR_DEREF,
  EXPR_CALL,
};
struct Expr {
  ExprType type;
  size_t start, end;
  std::variant<expr::Int *, expr::Bool *, expr::Var *, expr::Group *,
               expr::Binary *, expr::Ref *, expr::Deref *, expr::Call *>
      data;
  Type *expr_type = nullptr;

public:
  std::string to_string() const {
    switch (type) {
    case EXPR_INT: {
      auto x = std::get<expr::Int *>(data);
      return std::format("{}", x->value);
    }

    case EXPR_BOOL: {
      auto x = std::get<expr::Bool *>(data);
      return std::format("{}", x->value ? "true" : "false");
    }

    case EXPR_VAR: {
      auto x = std::get<expr::Var *>(data);
      return std::format("{}", x->var.id_value);
    }

    case EXPR_GROUP: {
      auto x = std::get<expr::Group *>(data);
      return std::format("({})", x->expr->to_string());
    }

    case EXPR_BINARY: {
      auto x = std::get<expr::Binary *>(data);
      return std::format("{} {} {}", x->lhs->to_string(),
                         binop_to_string(x->op), x->rhs->to_string());
    }

    case EXPR_REF: {
      auto x = std::get<expr::Ref *>(data);
      return std::format("&{}", x->expr->to_string());
    }

    case EXPR_DEREF: {
      auto x = std::get<expr::Deref *>(data);
      return std::format("*{}", x->expr->to_string());
    }

    case EXPR_CALL: {
      auto x = std::get<expr::Call *>(data);
      std::string out = std::format("{}(", x->name.id_value);
      for (size_t i = 0; i < x->arguments.size(); i++) {
        out += x->arguments[i]->to_string();
        if (i + 1 < x->arguments.size())
          out += ", ";
      }
      out += ")";
      return out;
    }
    }
  }

  bool is_lvalue() const {
    switch (type) {
    case EXPR_VAR:
    case EXPR_DEREF:  return true;

    case EXPR_INT:
    case EXPR_BOOL:
    case EXPR_GROUP:
    case EXPR_BINARY:
    case EXPR_REF:
    case EXPR_CALL:   return false;
    }
  }
};

namespace type {

  struct Ptr {
    Type *inner;
  };

} // namespace type

enum TypeType { TYPE_VOID, TYPE_BOOL, TYPE_INT, TYPE_PTR };
struct Type {
  TypeType type;
  size_t start, end;
  std::variant<type::Ptr *> data;

public:
  std::string to_string() const {
    switch (type) {
    case TYPE_VOID: return "void";
    case TYPE_BOOL: return "bool";
    case TYPE_INT:  return "int";
    case TYPE_PTR:  {
      Type *inner = std::get<type::Ptr *>(data)->inner;
      return std::format("*{}", inner->to_string());
    }
    }
  }

  bool is_pointer_to(Type *actual) {
    if (type != TYPE_PTR)
      return false;

    auto *ptr_data = std::get_if<type::Ptr *>(&data);
    if (!ptr_data || !*ptr_data || !(*ptr_data)->inner)
      return false;

    Type *pointee = (*ptr_data)->inner;
    return pointee->type == actual->type;
  }
};
