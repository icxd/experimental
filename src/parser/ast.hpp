#pragma once

#include <utility>

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

  struct Const {
    Token name;
    Type *type;
    Expr *value;
  };

  struct Proc {
    Token name;
    std::vector<Param> params;
    std::optional<Type *> ret_type;
    Linkage linkage;
    bool is_comptime = false;
    Block body;
    std::string codegen_name;
  };

  struct ComptimeBlock {
    std::vector<Decl *> decls;
  };

  struct When {
    Expr *cond;
    std::vector<Decl *> true_block;
    std::vector<Decl *> false_block;
  };

  struct Import {
    Token path;
  };

  struct StructField {
    Token name;
    Type *type;
  };

  struct Struct {
    Token name;
    std::vector<StructField> fields;
  };

} // namespace decl

enum DeclType {
  DECL_CONST,
  DECL_PROC,
  DECL_WHEN,
  DECL_IMPORT,
  DECL_COMPTIME_BLOCK,
  DECL_STRUCT,
};
struct Decl {
  DeclType type;
  size_t start, end;
  std::variant<decl::Const *, decl::Proc *, decl::When *, decl::Import *,
               decl::ComptimeBlock *, decl::Struct *>
      data;
};

namespace stmt {

  struct Block {
    std::vector<Stmt *> stmts;
    bool scoped = true;
  };

  struct Var {
    Token name;
    Type *type = nullptr; // nullptr = infer from initializer
    std::optional<Expr *> value = std::nullopt;
  };

  struct Return {
    std::optional<Expr *> value;
  };

  struct If {
    Expr *cond;
    std::vector<Stmt *> then_block;
    std::vector<Stmt *> else_block;
  };

  struct Assign {
    Expr *target;
    Expr *value;
  };

  struct While {
    Expr *cond;
    std::vector<Stmt *> body;
  };

  struct For {
    Stmt *init;
    Expr *cond;
    Stmt *step;
    std::vector<Stmt *> body;
  };

  struct Break {};

  struct Continue {};

  struct When {
    Expr *cond;
    std::vector<Stmt *> true_block;
    std::vector<Stmt *> false_block;
  };

  struct ExprStmt {
    Expr *expr;
  };

} // namespace stmt

enum StmtType {
  STMT_BLOCK,
  STMT_VAR,
  STMT_RETURN,
  STMT_IF,
  STMT_ASSIGN,
  STMT_WHILE,
  STMT_FOR,
  STMT_BREAK,
  STMT_CONTINUE,
  STMT_WHEN,
  STMT_EXPR,
};
struct Stmt {
  StmtType type;
  size_t start, end;
  std::variant<stmt::Block *, stmt::Var *, stmt::Return *, stmt::If *,
               stmt::Assign *, stmt::While *, stmt::For *, stmt::Break *,
               stmt::Continue *, stmt::When *, stmt::ExprStmt *>
      data;
};

namespace expr {

  struct Int {
    int64_t value;
  };

  struct Bool {
    bool value;
  };

  struct Var {
    std::optional<Token> module;
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
      BINOP_AND,
      BINOP_OR,
    } op;
    Expr *rhs;
  };

  struct Not {
    Expr *expr;
  };

  struct Ref {
    Expr *expr;
  };

  struct Deref {
    Expr *expr;
  };

  struct Call {
    std::optional<Token> module;
    std::optional<Token> name;
    Expr *callee = nullptr;
    std::optional<std::string> resolved_module;
    std::optional<std::string> resolved_codegen_name;
    std::vector<Expr *> arguments;
    Expr *receiver = nullptr;
  };

  struct TupleLit {
    std::vector<Expr *> elements;
  };

  struct ComptimeCall {
    Token name;
    std::vector<Expr *> arguments;
  };

  struct Field {
    Expr *base;
    Token field;
  };

  struct StructLitField {
    Token name;
    Expr *value;
  };

  struct StructLit {
    Token type_name;
    std::vector<StructLitField> fields;
  };

  struct String {
    Token value;
  };

  struct Sizeof {
    Type *type;
  };

  struct Cast {
    Type *target;
    Expr *expr;
  };

  struct Index {
    Expr *base;
    Expr *index;
  };

  struct ProcLit {
    std::vector<Param> params;
    Type *ret_type;
    Block body;
    std::string codegen_name;
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
  case expr::Binary::BINOP_AND:   return "&&";
  case expr::Binary::BINOP_OR:    return "||";
  }
  std::unreachable();
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
  EXPR_COMPTIME_CALL,
  EXPR_NOT,
  EXPR_FIELD,
  EXPR_STRUCT_LIT,
  EXPR_STRING,
  EXPR_SIZEOF,
  EXPR_CAST,
  EXPR_INDEX,
  EXPR_PROC_LIT,
  EXPR_TUPLE,
};
struct Expr {
  ExprType type;
  size_t start, end;
  std::variant<expr::Int *, expr::Bool *, expr::Var *, expr::Group *,
               expr::Binary *, expr::Ref *, expr::Deref *, expr::Call *,
               expr::ComptimeCall *, expr::Not *, expr::Field *,
               expr::StructLit *, expr::String *, expr::Sizeof *,
               expr::Cast *, expr::Index *, expr::ProcLit *, expr::TupleLit *>
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
      if (x->module.has_value())
        return std::format("{}:{}", x->module->id_value, x->var.id_value);
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
      std::string out;
      if (x->callee != nullptr) {
        out = x->callee->to_string();
      } else if (x->name.has_value()) {
        if (x->module.has_value())
          out = std::format("{}:{}", x->module->id_value, x->name->id_value);
        else
          out = std::format("{}", x->name->id_value);
      }
      out += "(";
      for (size_t i = 0; i < x->arguments.size(); i++) {
        out += x->arguments[i]->to_string();
        if (i + 1 < x->arguments.size())
          out += ", ";
      }
      out += ")";
      return out;
    }

    case EXPR_COMPTIME_CALL: {
      auto x = std::get<expr::ComptimeCall *>(data);
      std::string out = std::format("comptime {}", x->name.id_value);
      out += "(";
      for (size_t i = 0; i < x->arguments.size(); i++) {
        out += x->arguments[i]->to_string();
        if (i + 1 < x->arguments.size())
          out += ", ";
      }
      out += ")";
      return out;
    }

    case EXPR_NOT: {
      auto x = std::get<expr::Not *>(data);
      return std::format("!{}", x->expr->to_string());
    }

    case EXPR_FIELD: {
      auto x = std::get<expr::Field *>(data);
      return std::format("{}.{}", x->base->to_string(), x->field.id_value);
    }

    case EXPR_STRUCT_LIT: {
      auto x = std::get<expr::StructLit *>(data);
      std::string out = std::format("{}{{", x->type_name.id_value);
      for (size_t i = 0; i < x->fields.size(); i++) {
        out += std::format("{} = {}", x->fields[i].name.id_value,
                           x->fields[i].value->to_string());
        if (i + 1 < x->fields.size())
          out += ", ";
      }
      out += "}";
      return out;
    }

    case EXPR_STRING: {
      auto x = std::get<expr::String *>(data);
      return std::format("\"{}\"", x->value.string_value);
    }

    case EXPR_SIZEOF:
      return "sizeof(...)";

    case EXPR_CAST: {
      auto x = std::get<expr::Cast *>(data);
      return std::format("cast(...) {}", x->expr->to_string());
    }

    case EXPR_INDEX: {
      auto x = std::get<expr::Index *>(data);
      return std::format("{}[{}]", x->base->to_string(),
                         x->index->to_string());
    }

    case EXPR_PROC_LIT:
      return "proc { ... }";

    case EXPR_TUPLE: {
      auto *tuple = std::get<expr::TupleLit *>(data);
      std::string out = "(";
      for (size_t i = 0; i < tuple->elements.size(); i++) {
        out += tuple->elements[i]->to_string();
        if (i + 1 < tuple->elements.size())
          out += ", ";
      }
      out += ")";
      return out;
    }
    }
    std::unreachable();
  }

  bool is_lvalue() const {
    switch (type) {
    case EXPR_VAR:
    case EXPR_DEREF:
    case EXPR_FIELD:
    case EXPR_INDEX: return true;

    case EXPR_INT:
    case EXPR_BOOL:
    case EXPR_GROUP:
    case EXPR_BINARY:
    case EXPR_REF:
    case EXPR_CALL:
    case EXPR_COMPTIME_CALL:
    case EXPR_NOT:
    case EXPR_STRUCT_LIT:
    case EXPR_STRING:
    case EXPR_SIZEOF:
    case EXPR_CAST:
    case EXPR_PROC_LIT:
    case EXPR_TUPLE: return false;
    }
    std::unreachable();
  }
};

namespace type {

  struct Ptr {
    Type *inner;
  };

  struct Struct {
    std::string_view name;
  };

  struct Proc {
    std::vector<Param> params;
    Type *ret_type;
  };

  struct Tuple {
    std::vector<Type *> elements;
  };

} // namespace type

enum TypeType {
  TYPE_VOID,
  TYPE_BOOL,
  TYPE_INT,
  TYPE_BYTE,
  TYPE_PTR,
  TYPE_STRUCT,
  TYPE_PROC,
  TYPE_TUPLE,
};
struct Type {
  TypeType type;
  size_t start, end;
  std::variant<type::Ptr *, type::Struct *, type::Proc *, type::Tuple *> data;

public:
  std::string to_string() const {
    switch (type) {
    case TYPE_VOID: return "void";
    case TYPE_BOOL: return "bool";
    case TYPE_INT:  return "int";
    case TYPE_BYTE: return "byte";
    case TYPE_PTR:  {
      Type *inner = std::get<type::Ptr *>(data)->inner;
      return std::format("*{}", inner->to_string());
    }
    case TYPE_STRUCT:
      return std::string(std::get<type::Struct *>(data)->name);
    case TYPE_PROC: {
      auto *proc = std::get<type::Proc *>(data);
      std::string sig = "proc(";
      for (size_t i = 0; i < proc->params.size(); i++) {
        if (i > 0)
          sig += ", ";
        sig += std::string(proc->params[i].name.id_value) + " " +
               proc->params[i].type->to_string();
      }
      sig += ") " + proc->ret_type->to_string();
      return sig;
    }
    case TYPE_TUPLE: {
      auto *tuple = std::get<type::Tuple *>(data);
      std::string out = "(";
      for (size_t i = 0; i < tuple->elements.size(); i++) {
        out += tuple->elements[i]->to_string();
        if (i + 1 < tuple->elements.size())
          out += ", ";
      }
      out += ")";
      return out;
    }
    }
    std::unreachable();
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

  // Struct type used for field lookup; auto-derefs one level of pointer.
  Type *as_struct_for_field_access() {
    if (type == TYPE_STRUCT)
      return this;
    if (type == TYPE_PTR) {
      Type *inner = std::get<type::Ptr *>(data)->inner;
      if (inner->type == TYPE_STRUCT)
        return inner;
    }
    return nullptr;
  }
};
