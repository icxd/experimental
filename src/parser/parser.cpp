#include <common.hpp>
#include <lexer/token.hpp>
#include <parser/ast.hpp>
#include <parser/parser.hpp>

ErrorOr<std::vector<Decl *>> Parser::parse_decls() {
  std::vector<Decl *> decls{};
  while (_pos < _tokens.size()) {
    if (peek().type == TOK_EOF)
      break;

    Decl *decl = try$(parse_decl());
    decls.push_back(decl);
  }
  return decls;
}

ErrorOr<Decl *> Parser::parse_decl() {
  if (peek().type == TOK_PROC ||
      (peek().type == TOK_EXTERN && peek(1).type == TOK_PROC)) {
    size_t start = peek().start;
    Linkage linkage = LINK_INTERN;
    if (peek().type == TOK_EXTERN) {
      linkage = LINK_EXTERN;
      try$(expect(TOK_EXTERN, "Expected `extern`"));
    }
    Token proc = try$(expect(TOK_PROC, "Expected `proc`"));
    Token id = try$(expect(TOK_ID, "Expected an identifier after `proc`"));
    Token oparen = try$(expect(TOK_OPAREN, "Expected ( after identifier"));
    std::vector<Param> params{};
    while (_pos < _tokens.size() && peek().type != TOK_CPAREN) {
      Token id = try$(expect(TOK_ID, "Expected an identifier"));
      Type *type = try$(parse_type());
      params.push_back(Param{id, type});

      if (peek().type == TOK_COMMA)
        consume();
      else
        break;
    }
    Token cparen = try$(expect(TOK_CPAREN, "Expected ) after parameters"));
    Type *ret_type = try$(parse_type());

    Block body{};
    if (linkage == LINK_INTERN)
      body = try$(parse_block());
    else
      try$(expect(TOK_SEMICOLON, "Expected ; after return type"));


    return new Decl{.type = DECL_PROC,
                    .start = start,
                    .end = previous().end,
                    .data = new decl::Proc{
                        .name = id,
                        .params = params,
                        .ret_type = ret_type,
                        .linkage = linkage,
                        .body = body,
                    }};
  } else if (peek().type == TOK_CONST) {
    Token const_ = try$(expect(TOK_CONST, "Expected `const`"));
    Token id = try$(expect(TOK_ID, "Expected an identifier after `const`"));
    Type *type = try$(parse_type());
    Token eq = try$(expect(TOK_EQ, "Expected = after type"));
    Expr *value = try$(parse_expr());
    Token semi = try$(expect(TOK_SEMICOLON, "Expected ; after value"));

    return new Decl{.type = DECL_CONST,
                    .start = const_.start,
                    .end = semi.end,
                    .data = new decl::Const{
                        .name = id,
                        .type = type,
                        .value = value,
                    }};
  } else if (peek().type == TOK_WHEN) {
    Token when = try$(expect(TOK_WHEN, "Expected `when`"));
    Expr *condition = try$(parse_expr());

    std::vector<Decl *> when_block{};
    Token obrace = try$(expect(TOK_OBRACE, "Expected {"));
    while (_pos < _tokens.size() && peek().type != TOK_CBRACE)
      when_block.push_back(try$(parse_decl()));
    Token cbrace = try$(expect(TOK_CBRACE, "Expected }"));

    std::vector<Decl *> else_block{};
    if (peek().type == TOK_ELSE) {
      Token else_ = try$(expect(TOK_ELSE, "Expected `else`"));

      Token obrace = try$(expect(TOK_OBRACE, "Expected {"));
      while (_pos < _tokens.size() && peek().type != TOK_CBRACE)
        else_block.push_back(try$(parse_decl()));
      Token cbrace = try$(expect(TOK_CBRACE, "Expected }"));
    }

    return new Decl{.type = DECL_WHEN,
                    .start = when.start,
                    .end = previous().end,
                    .data = new decl::When{condition, when_block, else_block}};
  } else {
    return std::unexpected(
        Error("Expected `proc` or `when`", peek().start, peek().end));
  }
}

ErrorOr<Stmt *> Parser::parse_stmt() {
  if (peek().type == TOK_OBRACE) {
    std::vector<Stmt *> stmts = try$(parse_block());
    return new Stmt{.type = STMT_BLOCK,
                    .start = stmts.front()->start,
                    .end = stmts.back()->end,
                    .data = new stmt::Block{
                        .stmts = stmts,
                    }};
  } else if (peek().type == TOK_VAR) {
    Token var = try$(expect(TOK_VAR, "Expected `var`"));
    Token id = try$(expect(TOK_ID, "Expected an identifier after `var`"));
    Type *type = try$(parse_type());
    Token eq = try$(expect(TOK_EQ, "Expected = after type"));
    Expr *value = try$(parse_expr());
    Token semi = try$(expect(TOK_SEMICOLON, "Expected ; after value"));
    return new Stmt{.type = STMT_VAR,
                    .start = var.start,
                    .end = semi.end,
                    .data = new stmt::Var{
                        .name = id,
                        .type = type,
                        .value = value,
                    }};
  } else if (peek().type == TOK_RETURN) {
    Token return_ = try$(expect(TOK_RETURN, "Expected `return`"));
    std::optional<Expr *> expr = std::nullopt;
    if (peek().type != TOK_SEMICOLON)
      expr = std::make_optional(try$(parse_expr()));
    Token semi = try$(expect(TOK_SEMICOLON, "Expected ;"));
    return new Stmt{.type = STMT_RETURN,
                    .start = return_.start,
                    .end = semi.end,
                    .data = new stmt::Return{.value = expr}};
  } else {
    return std::unexpected(
        Error("Expected a statement", peek().start, peek().end));
  }
}

bool is_binary_operator(Token tok) {
  switch (tok.type) {
  case TOK_PLUS:
  case TOK_MINUS:
  case TOK_STAR:
  case TOK_SLASH:
  case TOK_EQEQ:
  case TOK_NEQ:
  case TOK_LT:
  case TOK_LTE:
  case TOK_GT:
  case TOK_GTE:   return true;
  default:        return false;
  }
}

size_t get_operator_precedence(Token tok) {
  switch (tok.type) {
  case TOK_STAR:
  case TOK_SLASH: return 20; // Multiplication / Division

  case TOK_PLUS:
  case TOK_MINUS: return 15; // Addition / Subtraction

  case TOK_EQEQ:
  case TOK_NEQ:
  case TOK_LT:
  case TOK_LTE:
  case TOK_GT:
  case TOK_GTE:   return 10; // Comparisons

  default:        return 0; // Lowest precedence / unknown case TOK_PLUS:
  }
}

expr::Binary::BinOp tok_to_binop(Token tok) {
  switch (tok.type) {
  case TOK_PLUS:  return expr::Binary::BINOP_PLUS;
  case TOK_MINUS: return expr::Binary::BINOP_MINUS;
  case TOK_STAR:  return expr::Binary::BINOP_STAR;
  case TOK_SLASH: return expr::Binary::BINOP_SLASH;
  case TOK_EQEQ:  return expr::Binary::BINOP_EQ;
  case TOK_NEQ:   return expr::Binary::BINOP_NEQ;
  case TOK_LT:    return expr::Binary::BINOP_LT;
  case TOK_LTE:   return expr::Binary::BINOP_LTE;
  case TOK_GT:    return expr::Binary::BINOP_GT;
  case TOK_GTE:   return expr::Binary::BINOP_GTE;
  default:        PANIC("unreachable");
  }
}

ErrorOr<Expr *> Parser::parse_expr(size_t max_prec) {
  Expr *lhs = try$(parse_primary_expr());

  for (;;) {
    Token op = peek();

    if (!is_binary_operator(op))
      break;

    size_t prec = get_operator_precedence(op);
    if (prec >= max_prec)
      break;

    // Left-associative: use prec + 1
    consume(); // consume operator

    Expr *rhs = try$(parse_expr(prec + 1));

    lhs = new Expr{.type = EXPR_BINARY,
                   .start = lhs->start,
                   .end = rhs->end,
                   .data = new expr::Binary{
                       .lhs = lhs,
                       .op = tok_to_binop(op),
                       .rhs = rhs,
                   }};
  }

  return lhs;
}

ErrorOr<Expr *> Parser::parse_primary_expr() {
  if (peek().type == TOK_ID) {
    Token var = consume();
    if (var.id_value == "true")
      return new Expr{.type = EXPR_BOOL,
                      .start = var.start,
                      .end = var.end,
                      .data = new expr::Bool{.value = true}};
    else if (var.id_value == "false")
      return new Expr{.type = EXPR_BOOL,
                      .start = var.start,
                      .end = var.end,
                      .data = new expr::Bool{.value = false}};
    else {
      if (peek().type == TOK_OPAREN) {
        Token oparen = try$(expect(TOK_OPAREN, "Expected ( after identifier"));
        std::vector<Expr *> args{};
        while (_pos < _tokens.size() && peek().type != TOK_CPAREN) {
          args.push_back(try$(parse_expr()));
          if (peek().type == TOK_COMMA)
            try$(expect(TOK_COMMA, "Expected , after argument"));
          else
            break;
        }
        Token cparen = try$(expect(TOK_CPAREN, "Expected ) after arguments"));

        return new Expr{.type = EXPR_CALL,
                        .start = var.start,
                        .end = cparen.end,
                        .data = new expr::Call{var, args}};
      }

      return new Expr{.type = EXPR_VAR,
                      .start = var.start,
                      .end = var.end,
                      .data = new expr::Var{.var = var}};
    }
  } else if (peek().type == TOK_INT) {
    Token int_ = consume();
    return new Expr{.type = EXPR_INT,
                    .start = int_.start,
                    .end = int_.end,
                    .data = new expr::Int{.value = int_.int_value}};
  } else if (peek().type == TOK_AMPERSAND) {
    Token ampersand = consume();
    Expr *expr = try$(parse_expr());
    return new Expr{.type = EXPR_REF,
                    .start = ampersand.start,
                    .end = expr->end,
                    .data = new expr::Ref{.expr = expr}};
  } else if (peek().type == TOK_STAR) {
    Token star = consume();
    Expr *expr = try$(parse_expr());
    return new Expr{.type = EXPR_DEREF,
                    .start = star.start,
                    .end = expr->end,
                    .data = new expr::Deref{.expr = expr}};
  } else {
    return std::unexpected(
        Error("Expected a expression", peek().start, peek().end));
  }
}

ErrorOr<Type *> Parser::parse_type() {
  if (peek().type == TOK_ID) {
    if (peek().id_value == "bool") {
      Token bool_ = consume();
      return new Type{
          .type = TYPE_BOOL,
          .start = bool_.start,
          .end = bool_.end,
      };
    } else if (peek().id_value == "int") {
      Token int_ = consume();
      return new Type{
          .type = TYPE_INT,
          .start = int_.start,
          .end = int_.end,
      };
    } else if (peek().id_value == "void") {
      Token void_ = consume();
      return new Type{
          .type = TYPE_VOID,
          .start = void_.start,
          .end = void_.end,
      };
    } else {
      return std::unexpected(Error("User defined types are not supported yet",
                                   peek().start, peek().end));
    }
  } else if (peek().type == TOK_STAR) {
    Token star = consume();
    Type *inner = try$(parse_type());
    return new Type{
        .type = TYPE_PTR,
        .start = star.start,
        .end = inner->end,
        .data = new type::Ptr{inner},
    };
  } else {
    return std::unexpected(Error("Expected a type", peek().start, peek().end));
  }
}

ErrorOr<Stmt *> Parser::parse_block_stmt() {
  std::vector<Stmt *> block{};
  Token obrace = try$(expect(TOK_OBRACE, "Expected {"));
  while (_pos < _tokens.size() && peek().type != TOK_CBRACE)
    block.push_back(try$(parse_stmt()));
  Token cbrace = try$(expect(TOK_CBRACE, "Expected }"));
  return new Stmt{.type = STMT_BLOCK,
                  .start = obrace.start,
                  .end = cbrace.end,
                  .data = new stmt::Block{block}};
}

ErrorOr<std::vector<Stmt *>> Parser::parse_block() {
  std::vector<Stmt *> block{};
  try$(expect(TOK_OBRACE, "Expected {"));
  while (_pos < _tokens.size() && peek().type != TOK_CBRACE)
    block.push_back(try$(parse_stmt()));
  try$(expect(TOK_CBRACE, "Expected }"));
  return block;
}

ErrorOr<Token> Parser::expect(TokenType type, std::string message) {
  if (peek().type == type)
    return consume();

  return std::unexpected(Error(message, peek().start, peek().end));
}

Token Parser::consume() {
  Token token = _tokens.at(_pos);
  _pos++;
  return token;
}

Token Parser::peek(size_t offset) { return _tokens.at(_pos + offset); }

Token Parser::previous() { return _tokens.at(_pos - 1); }
