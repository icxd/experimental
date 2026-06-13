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
  if (peek().type == TOK_COMPTIME && peek(1).type == TOK_OBRACE) {
    Token comptime = try$(expect(TOK_COMPTIME, "Expected `comptime`"));
    std::vector<Decl *> block_decls{};
    Token obrace = try$(expect(TOK_OBRACE, "Expected {"));
    while (_pos < _tokens.size() && peek().type != TOK_CBRACE)
      block_decls.push_back(try$(parse_decl()));
    Token cbrace = try$(expect(TOK_CBRACE, "Expected }"));

    auto *block_data = _arena.create<decl::ComptimeBlock>(
        decl::ComptimeBlock{.decls = block_decls});

    return _arena.create<Decl>(Decl{.type = DECL_COMPTIME_BLOCK,
                                    .start = comptime.start,
                                    .end = cbrace.end,
                                    .data = block_data});
  }

  bool is_comptime = false;
  size_t decl_start = peek().start;
  if (peek().type == TOK_COMPTIME) {
    is_comptime = true;
    decl_start = peek().start;
    try$(expect(TOK_COMPTIME, "Expected `comptime`"));
  }

  if (peek().type == TOK_PROC ||
      (peek().type == TOK_EXTERN && peek(1).type == TOK_PROC)) {
    size_t start = is_comptime ? decl_start : peek().start;
    Linkage linkage = LINK_INTERN;
    if (peek().type == TOK_EXTERN) {
      if (is_comptime) {
        return std::unexpected(
            Error("`comptime` procedures cannot be `extern`", peek().start,
                  peek().end));
      }
      linkage = LINK_EXTERN;
      try$(expect(TOK_EXTERN, "Expected `extern`"));
    }
    Token proc = try$(expect(TOK_PROC, "Expected `proc`"));
    Token id = try$(expect(TOK_ID, "Expected an identifier after `proc`"));
    Token oparen = try$(expect(TOK_OPAREN, "Expected ( after identifier"));
    std::vector<Param> params{};
    while (_pos < _tokens.size() && peek().type != TOK_CPAREN) {
      std::vector<Token> names = try$(parse_name_list());
      Type *type = try$(parse_type());
      for (const Token &name: names)
        params.push_back(Param{name, type});

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

    auto *proc_data = _arena.create<decl::Proc>(decl::Proc{
        .name = id,
        .params = params,
        .ret_type = ret_type,
        .linkage = linkage,
        .is_comptime = is_comptime,
        .body = body,
    });

    return _arena.create<Decl>(Decl{.type = DECL_PROC,
                                    .start = start,
                                    .end = previous().end,
                                    .data = proc_data});
  } else if (peek().type == TOK_CONST) {
    Token const_ = try$(expect(TOK_CONST, "Expected `const`"));
    Token id = try$(expect(TOK_ID, "Expected an identifier after `const`"));
    Type *type = try$(parse_type());
    Token eq = try$(expect(TOK_EQ, "Expected = after type"));
    Expr *value = try$(parse_expr());
    Token semi = try$(expect(TOK_SEMICOLON, "Expected ; after value"));

    auto *const_data = _arena.create<decl::Const>(decl::Const{
        .name = id,
        .type = type,
        .value = value,
    });

    return _arena.create<Decl>(Decl{.type = DECL_CONST,
                                    .start = const_.start,
                                    .end = semi.end,
                                    .data = const_data});
  } else if (peek().type == TOK_WHEN) {
    Token when = try$(expect(TOK_WHEN, "Expected `when`"));
    Expr *condition = try$(parse_expr(0, false));

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

    auto *when_data = _arena.create<decl::When>(
        decl::When{condition, when_block, else_block});

    return _arena.create<Decl>(Decl{.type = DECL_WHEN,
                                    .start = when.start,
                                    .end = previous().end,
                                    .data = when_data});
  } else if (peek().type == TOK_IMPORT) {
    Token import = try$(expect(TOK_IMPORT, "Expected `import`"));
    Token path = try$(expect(TOK_STRING, "Expected import path string"));
    Token semi = try$(expect(TOK_SEMICOLON, "Expected `;` after import"));

    auto *import_data =
        _arena.create<decl::Import>(decl::Import{.path = path});

    return _arena.create<Decl>(Decl{.type = DECL_IMPORT,
                                    .start = import.start,
                                    .end = semi.end,
                                    .data = import_data});
  } else if (peek().type == TOK_STRUCT) {
    Token struct_ = try$(expect(TOK_STRUCT, "Expected `struct`"));
    Token struct_name = try$(expect(TOK_ID, "Expected struct name"));
    Token obrace = try$(expect(TOK_OBRACE, "Expected `{`"));
    std::vector<decl::StructField> fields{};
    while (_pos < _tokens.size() && peek().type != TOK_CBRACE) {
      std::vector<Token> names = try$(parse_name_list());
      Type *field_type = try$(parse_type());
      try$(expect(TOK_SEMICOLON, "Expected `;` after struct field"));
      for (const Token &field_name: names)
        fields.push_back(decl::StructField{field_name, field_type});
    }
    Token cbrace = try$(expect(TOK_CBRACE, "Expected `}`"));

    auto *struct_data = _arena.create<decl::Struct>(
        decl::Struct{.name = struct_name, .fields = fields});

    return _arena.create<Decl>(Decl{.type = DECL_STRUCT,
                                    .start = struct_.start,
                                    .end = cbrace.end,
                                    .data = struct_data});
  } else if (is_comptime) {
    return std::unexpected(
        Error("Expected `proc` or `{` after `comptime`", peek().start,
              peek().end));
  } else {
    return std::unexpected(
        Error("Expected `proc`, `const`, `struct`, `import`, `when`, or "
              "`comptime`",
              peek().start, peek().end));
  }
}

ErrorOr<Stmt *> Parser::parse_stmt() {
  if (peek().type == TOK_OBRACE) {
    std::vector<Stmt *> stmts = try$(parse_block());
    return _arena.create<Stmt>(Stmt{
        .type = STMT_BLOCK,
        .start = stmts.front()->start,
        .end = stmts.back()->end,
        .data = _arena.create<stmt::Block>(stmt::Block{.stmts = stmts}),
    });
  } else if (peek().type == TOK_VAR) {
    Token var = try$(expect(TOK_VAR, "Expected `var`"));
    std::vector<Stmt *> decls{};
    while (true) {
      Token id = try$(expect(TOK_ID, "Expected an identifier after `var`"));
      Type *type = try$(parse_type());
      std::optional<Expr *> value = std::nullopt;
      if (peek().type == TOK_EQ) {
        consume();
        value = try$(parse_expr());
      }

      decls.push_back(_arena.create<Stmt>(Stmt{
          .type = STMT_VAR,
          .start = var.start,
          .end = value.has_value() ? value.value()->end : type->end,
          .data = _arena.create<stmt::Var>(stmt::Var{
              .name = id,
              .type = type,
              .value = value,
          }),
      }));

      if (peek().type == TOK_COMMA) {
        consume();
        continue;
      }
      break;
    }
    Token semi = try$(expect(TOK_SEMICOLON, "Expected ; after variable declaration"));

    if (decls.size() == 1) {
      decls.front()->end = semi.end;
      return decls.front();
    }

    for (Stmt *stmt: decls)
      stmt->end = semi.end;

    return _arena.create<Stmt>(Stmt{
        .type = STMT_BLOCK,
        .start = var.start,
        .end = semi.end,
        .data = _arena.create<stmt::Block>(stmt::Block{.stmts = decls}),
    });
  } else if (peek().type == TOK_ID || peek().type == TOK_OPAREN ||
             peek().type == TOK_STAR) {
    size_t save = _pos;
    Expr *lhs = try$(parse_postfix_expr(false));
    if (peek().type == TOK_EQ && lhs->is_lvalue()) {
      consume();
      Expr *value = try$(parse_expr());
      Token semi =
          try$(expect(TOK_SEMICOLON, "Expected `;` after assignment"));
      return _arena.create<Stmt>(Stmt{
          .type = STMT_ASSIGN,
          .start = lhs->start,
          .end = semi.end,
          .data = _arena.create<stmt::Assign>(
              stmt::Assign{.target = lhs, .value = value}),
      });
    }
    _pos = save;
  }

  if (peek().type == TOK_WHILE) {
    Token while_ = try$(expect(TOK_WHILE, "Expected `while`"));
    Expr *cond = try$(parse_expr(0, false));
    std::vector<Stmt *> body = try$(parse_block());
    return _arena.create<Stmt>(Stmt{
        .type = STMT_WHILE,
        .start = while_.start,
        .end = previous().end,
        .data = _arena.create<stmt::While>(stmt::While{.cond = cond, .body = body}),
    });
  } else if (peek().type == TOK_FOR) {
    Token for_ = try$(expect(TOK_FOR, "Expected `for`"));
    Stmt *init = try$(parse_stmt());
    Expr *cond = try$(parse_expr(0, false));
    try$(expect(TOK_SEMICOLON, "Expected `;` after for-condition"));
    if (peek().type != TOK_ID || peek(1).type != TOK_EQ) {
      return std::unexpected(
          Error("For-loop step must be an assignment", peek().start, peek().end));
    }
    Token step_id = consume();
    try$(expect(TOK_EQ, "Expected `=`"));
    Expr *step_value = try$(parse_expr());
    if (peek().type == TOK_SEMICOLON)
      consume();
    Expr *step_target = _arena.create<Expr>(Expr{
        .type = EXPR_VAR,
        .start = step_id.start,
        .end = step_id.end,
        .data = _arena.create<expr::Var>(expr::Var{.var = step_id}),
    });
    auto *step_data = _arena.create<stmt::Assign>(
        stmt::Assign{.target = step_target, .value = step_value});
    Stmt *step = _arena.create<Stmt>(Stmt{
        .type = STMT_ASSIGN,
        .start = step_id.start,
        .end = previous().end,
        .data = step_data,
    });
    std::vector<Stmt *> body = try$(parse_block());
    return _arena.create<Stmt>(Stmt{
        .type = STMT_FOR,
        .start = for_.start,
        .end = previous().end,
        .data = _arena.create<stmt::For>(stmt::For{
            .init = init,
            .cond = cond,
            .step = step,
            .body = body,
        }),
    });
  } else if (peek().type == TOK_WHEN) {
    Token when = try$(expect(TOK_WHEN, "Expected `when`"));
    Expr *cond = try$(parse_expr(0, false));

    std::vector<Stmt *> when_block = try$(parse_block());
    std::vector<Stmt *> else_block{};
    if (peek().type == TOK_ELSE) {
      try$(expect(TOK_ELSE, "Expected `else`"));
      else_block = try$(parse_block());
    }

    return _arena.create<Stmt>(Stmt{
        .type = STMT_WHEN,
        .start = when.start,
        .end = previous().end,
        .data = _arena.create<stmt::When>(stmt::When{
            .cond = cond,
            .true_block = when_block,
            .false_block = else_block,
        }),
    });
  } else if (peek().type == TOK_IF) {
    Token if_ = try$(expect(TOK_IF, "Expected `if`"));
    Expr *cond = try$(parse_expr(0, false));
    std::vector<Stmt *> then_block = try$(parse_block());
    std::vector<Stmt *> else_block{};
    if (peek().type == TOK_ELSE) {
      try$(expect(TOK_ELSE, "Expected `else`"));
      else_block = try$(parse_block());
    }
    return _arena.create<Stmt>(Stmt{
        .type = STMT_IF,
        .start = if_.start,
        .end = previous().end,
        .data = _arena.create<stmt::If>(stmt::If{
            .cond = cond,
            .then_block = then_block,
            .else_block = else_block,
        }),
    });
  } else if (peek().type == TOK_RETURN) {
    Token return_ = try$(expect(TOK_RETURN, "Expected `return`"));
    std::optional<Expr *> expr = std::nullopt;
    if (peek().type != TOK_SEMICOLON)
      expr = std::make_optional(try$(parse_expr()));
    Token semi = try$(expect(TOK_SEMICOLON, "Expected ;"));
    return _arena.create<Stmt>(Stmt{
        .type = STMT_RETURN,
        .start = return_.start,
        .end = semi.end,
        .data = _arena.create<stmt::Return>(stmt::Return{.value = expr}),
    });
  } else if (peek().type == TOK_BREAK) {
    Token brk = try$(expect(TOK_BREAK, "Expected `break`"));
    try$(expect(TOK_SEMICOLON, "Expected `;` after `break`"));
    return _arena.create<Stmt>(Stmt{
        .type = STMT_BREAK,
        .start = brk.start,
        .end = previous().end,
        .data = _arena.create<stmt::Break>(stmt::Break{}),
    });
  } else if (peek().type == TOK_CONTINUE) {
    Token cont = try$(expect(TOK_CONTINUE, "Expected `continue`"));
    try$(expect(TOK_SEMICOLON, "Expected `;` after `continue`"));
    return _arena.create<Stmt>(Stmt{
        .type = STMT_CONTINUE,
        .start = cont.start,
        .end = previous().end,
        .data = _arena.create<stmt::Continue>(stmt::Continue{}),
    });
  } else if (peek().type == TOK_ID || peek().type == TOK_OPAREN ||
             peek().type == TOK_BANG || peek().type == TOK_STAR) {
    Expr *expr = try$(parse_expr());
    Token semi =
        try$(expect(TOK_SEMICOLON, "Expected `;` after expression statement"));
    return _arena.create<Stmt>(Stmt{
        .type = STMT_EXPR,
        .start = expr->start,
        .end = semi.end,
        .data = _arena.create<stmt::ExprStmt>(stmt::ExprStmt{.expr = expr}),
    });
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
  case TOK_GTE:
  case TOK_AMPAMP:
  case TOK_PIPEPIPE: return true;
  default:           return false;
  }
}

size_t get_operator_precedence(Token tok) {
  switch (tok.type) {
  case TOK_STAR:
  case TOK_SLASH: return 20;

  case TOK_PLUS:
  case TOK_MINUS: return 15;

  case TOK_PIPEPIPE: return 9;

  case TOK_AMPAMP: return 10;

  case TOK_EQEQ:
  case TOK_NEQ:
  case TOK_LT:
  case TOK_LTE:
  case TOK_GT:
  case TOK_GTE:   return 11;

  default:           return 0;
  }
}

expr::Binary::BinOp tok_to_binop(Token tok) {
  switch (tok.type) {
  case TOK_PLUS:     return expr::Binary::BINOP_PLUS;
  case TOK_MINUS:    return expr::Binary::BINOP_MINUS;
  case TOK_STAR:     return expr::Binary::BINOP_STAR;
  case TOK_SLASH:    return expr::Binary::BINOP_SLASH;
  case TOK_EQEQ:     return expr::Binary::BINOP_EQ;
  case TOK_NEQ:      return expr::Binary::BINOP_NEQ;
  case TOK_LT:       return expr::Binary::BINOP_LT;
  case TOK_LTE:      return expr::Binary::BINOP_LTE;
  case TOK_GT:       return expr::Binary::BINOP_GT;
  case TOK_GTE:      return expr::Binary::BINOP_GTE;
  case TOK_AMPAMP:   return expr::Binary::BINOP_AND;
  case TOK_PIPEPIPE: return expr::Binary::BINOP_OR;
  default:           PANIC("unreachable");
  }
}

ErrorOr<Expr *> Parser::parse_expr(size_t min_prec, bool allow_struct_lit) {
  Expr *lhs = try$(parse_postfix_expr(allow_struct_lit));

  for (;;) {
    Token op = peek();

    if (!is_binary_operator(op))
      break;

    size_t prec = get_operator_precedence(op);
    if (prec < min_prec)
      break;

    consume();

    Expr *rhs = try$(parse_expr(prec + 1, allow_struct_lit));

    auto *binary_data = _arena.create<expr::Binary>(expr::Binary{
        .lhs = lhs,
        .op = tok_to_binop(op),
        .rhs = rhs,
    });

    lhs = _arena.create<Expr>(Expr{
        .type = EXPR_BINARY,
        .start = lhs->start,
        .end = rhs->end,
        .data = binary_data,
    });
  }

  return lhs;
}

ErrorOr<Expr *> Parser::parse_postfix_expr(bool allow_struct_lit) {
  Expr *expr = try$(parse_primary_expr(allow_struct_lit));

  for (;;) {
    if (peek().type == TOK_DOT) {
      Token dot = consume();
      Token field = try$(expect(TOK_ID, "Expected field name after `.`"));
      if (peek().type == TOK_OPAREN) {
        Token oparen = try$(expect(TOK_OPAREN, "Expected `(`"));
        std::vector<Expr *> args{};
        while (_pos < _tokens.size() && peek().type != TOK_CPAREN) {
          args.push_back(try$(parse_expr(0, allow_struct_lit)));
          if (peek().type == TOK_COMMA)
            try$(expect(TOK_COMMA, "Expected `,` after argument"));
          else
            break;
        }
        Token cparen = try$(expect(TOK_CPAREN, "Expected `)`"));
        expr = _arena.create<Expr>(Expr{
            .type = EXPR_CALL,
            .start = expr->start,
            .end = cparen.end,
            .data = _arena.create<expr::Call>(expr::Call{
                .name = field,
                .arguments = args,
                .receiver = expr,
            }),
        });
        continue;
      }

      auto *field_data =
          _arena.create<expr::Field>(expr::Field{.base = expr, .field = field});
      expr = _arena.create<Expr>(Expr{
          .type = EXPR_FIELD,
          .start = expr->start,
          .end = field.end,
          .data = field_data,
      });
      continue;
    }

    if (peek().type == TOK_OBRACKET) {
      consume();
      Expr *index = try$(parse_expr(0, allow_struct_lit));
      Token cbracket = try$(expect(TOK_CBRACKET, "Expected `]`"));
      auto *index_data = _arena.create<expr::Index>(
          expr::Index{.base = expr, .index = index});
      expr = _arena.create<Expr>(Expr{
          .type = EXPR_INDEX,
          .start = expr->start,
          .end = cbracket.end,
          .data = index_data,
      });
      continue;
    }

    if (peek().type == TOK_OPAREN) {
      Token oparen = try$(expect(TOK_OPAREN, "Expected `(`"));
      std::vector<Expr *> args{};
      while (_pos < _tokens.size() && peek().type != TOK_CPAREN) {
        args.push_back(try$(parse_expr(0, allow_struct_lit)));
        if (peek().type == TOK_COMMA)
          try$(expect(TOK_COMMA, "Expected `,` after argument"));
        else
          break;
      }
      Token cparen = try$(expect(TOK_CPAREN, "Expected `)`"));
      expr = _arena.create<Expr>(Expr{
          .type = EXPR_CALL,
          .start = expr->start,
          .end = cparen.end,
          .data = _arena.create<expr::Call>(
              expr::Call{.callee = expr, .arguments = args}),
      });
      continue;
    }

    break;
  }

  return expr;
}

ErrorOr<Expr *> Parser::parse_primary_expr(bool allow_struct_lit) {
  if (peek().type == TOK_OPAREN) {
    Token oparen = try$(expect(TOK_OPAREN, "Expected `(`"));
    if (peek().type == TOK_CPAREN) {
      return std::unexpected(
          Error("Expected expression in parentheses", oparen.start, peek().end));
    }
    Expr *first = try$(parse_expr(0, allow_struct_lit));
    if (peek().type == TOK_COMMA) {
      std::vector<Expr *> elements{first};
      while (peek().type == TOK_COMMA) {
        consume();
        if (peek().type == TOK_CPAREN) {
          return std::unexpected(
              Error("Expected expression after `,` in tuple", peek().start,
                    peek().end));
        }
        elements.push_back(try$(parse_expr(0, allow_struct_lit)));
      }
      Token cparen = try$(expect(TOK_CPAREN, "Expected `)`"));
      return _arena.create<Expr>(Expr{
          .type = EXPR_TUPLE,
          .start = oparen.start,
          .end = cparen.end,
          .data = _arena.create<expr::TupleLit>(
              expr::TupleLit{.elements = elements}),
      });
    }
    Token cparen = try$(expect(TOK_CPAREN, "Expected `)`"));
    return _arena.create<Expr>(Expr{
        .type = EXPR_GROUP,
        .start = oparen.start,
        .end = cparen.end,
        .data = _arena.create<expr::Group>(expr::Group{.expr = first}),
    });
  } else if (peek().type == TOK_BANG) {
    Token bang = consume();
    Expr *expr = try$(parse_postfix_expr(allow_struct_lit));
    return _arena.create<Expr>(Expr{
        .type = EXPR_NOT,
        .start = bang.start,
        .end = expr->end,
        .data = _arena.create<expr::Not>(expr::Not{.expr = expr}),
    });
  } else if (peek().type == TOK_COMPTIME) {
    Token comptime = try$(expect(TOK_COMPTIME, "Expected `comptime`"));
    Token name = try$(expect(TOK_ID, "Expected procedure name after `comptime`"));
    Token oparen = try$(expect(TOK_OPAREN, "Expected `(`"));
    std::vector<Expr *> args{};
    while (_pos < _tokens.size() && peek().type != TOK_CPAREN) {
      args.push_back(try$(parse_expr(0, allow_struct_lit)));
      if (peek().type == TOK_COMMA)
        try$(expect(TOK_COMMA, "Expected `,` after argument"));
      else
        break;
    }
    Token cparen = try$(expect(TOK_CPAREN, "Expected `)`"));
    return _arena.create<Expr>(Expr{
        .type = EXPR_COMPTIME_CALL,
        .start = comptime.start,
        .end = cparen.end,
        .data = _arena.create<expr::ComptimeCall>(
            expr::ComptimeCall{.name = name, .arguments = args}),
    });
  } else if (peek().type == TOK_PROC) {
    Token proc = try$(expect(TOK_PROC, "Expected `proc`"));
    auto sig = try$(parse_proc_signature());
    Block body = try$(parse_block());
    auto *lit_data = _arena.create<expr::ProcLit>(expr::ProcLit{
        .params = std::move(sig.first),
        .ret_type = sig.second,
        .body = body,
    });
    return _arena.create<Expr>(Expr{
        .type = EXPR_PROC_LIT,
        .start = proc.start,
        .end = body.empty() ? sig.second->end : body.back()->end,
        .data = lit_data,
    });
  } else if (peek().type == TOK_ID) {
    Token var = consume();
    if (var.id_value == "true")
      return _arena.create<Expr>(Expr{
          .type = EXPR_BOOL,
          .start = var.start,
          .end = var.end,
          .data = _arena.create<expr::Bool>(expr::Bool{.value = true}),
      });
    else if (var.id_value == "false")
      return _arena.create<Expr>(Expr{
          .type = EXPR_BOOL,
          .start = var.start,
          .end = var.end,
          .data = _arena.create<expr::Bool>(expr::Bool{.value = false}),
      });
    else {
      std::optional<Token> module = std::nullopt;
      Token name = var;

      if (peek().type == TOK_COLON) {
        consume();
        module = var;
        name = try$(expect(TOK_ID, "Expected procedure name after `:`"));
      }

      if (peek().type == TOK_OPAREN && module.has_value()) {
        Token oparen = try$(expect(TOK_OPAREN, "Expected ( after identifier"));
        std::vector<Expr *> args{};
        while (_pos < _tokens.size() && peek().type != TOK_CPAREN) {
          args.push_back(try$(parse_expr(0, allow_struct_lit)));
          if (peek().type == TOK_COMMA)
            try$(expect(TOK_COMMA, "Expected , after argument"));
          else
            break;
        }
        Token cparen = try$(expect(TOK_CPAREN, "Expected ) after arguments"));

        return _arena.create<Expr>(Expr{
            .type = EXPR_CALL,
            .start = var.start,
            .end = cparen.end,
            .data = _arena.create<expr::Call>(
                expr::Call{.module = module, .name = name, .arguments = args}),
        });
      }

      if (peek().type == TOK_OBRACE && allow_struct_lit) {
        Token obrace = try$(expect(TOK_OBRACE, "Expected `{`"));
        std::vector<expr::StructLitField> fields{};
        while (_pos < _tokens.size() && peek().type != TOK_CBRACE) {
          Token field_name =
              try$(expect(TOK_ID, "Expected field name in struct literal"));
          try$(expect(TOK_EQ, "Expected `=` after field name"));
          Expr *value = try$(parse_expr(0, allow_struct_lit));
          fields.push_back(expr::StructLitField{field_name, value});
          if (peek().type == TOK_COMMA)
            consume();
          else
            break;
        }
        Token cbrace = try$(expect(TOK_CBRACE, "Expected `}`"));

        return _arena.create<Expr>(Expr{
            .type = EXPR_STRUCT_LIT,
            .start = var.start,
            .end = cbrace.end,
            .data = _arena.create<expr::StructLit>(
                expr::StructLit{.type_name = name, .fields = fields}),
        });
      }

      return _arena.create<Expr>(Expr{
          .type = EXPR_VAR,
          .start = var.start,
          .end = name.end,
          .data = _arena.create<expr::Var>(
              expr::Var{.module = module, .var = name}),
      });
    }
  } else if (peek().type == TOK_CAST) {
    Token cast_ = try$(expect(TOK_CAST, "Expected `cast`"));
    try$(expect(TOK_OPAREN, "Expected `(` after `cast`"));
    Type *target = try$(parse_type());
    Token cparen = try$(expect(TOK_CPAREN, "Expected `)` after cast type"));
    Expr *value = try$(parse_postfix_expr(allow_struct_lit));
    return _arena.create<Expr>(Expr{
        .type = EXPR_CAST,
        .start = cast_.start,
        .end = value->end,
        .data = _arena.create<expr::Cast>(
            expr::Cast{.target = target, .expr = value}),
    });
  } else if (peek().type == TOK_SIZEOF) {
    Token sizeof_ = try$(expect(TOK_SIZEOF, "Expected `sizeof`"));
    try$(expect(TOK_OPAREN, "Expected `(` after `sizeof`"));
    Type *type = try$(parse_type());
    Token cparen = try$(expect(TOK_CPAREN, "Expected `)` after type"));
    return _arena.create<Expr>(Expr{
        .type = EXPR_SIZEOF,
        .start = sizeof_.start,
        .end = cparen.end,
        .data = _arena.create<expr::Sizeof>(expr::Sizeof{.type = type}),
    });
  } else if (peek().type == TOK_INT) {
    Token int_ = consume();
    return _arena.create<Expr>(Expr{
        .type = EXPR_INT,
        .start = int_.start,
        .end = int_.end,
        .data = _arena.create<expr::Int>(expr::Int{.value = int_.int_value}),
    });
  } else if (peek().type == TOK_STRING) {
    Token str = consume();
    return _arena.create<Expr>(Expr{
        .type = EXPR_STRING,
        .start = str.start,
        .end = str.end,
        .data = _arena.create<expr::String>(expr::String{.value = str}),
    });
  } else if (peek().type == TOK_AMPERSAND) {
    Token ampersand = consume();
    Expr *expr = try$(parse_expr(0, allow_struct_lit));
    return _arena.create<Expr>(Expr{
        .type = EXPR_REF,
        .start = ampersand.start,
        .end = expr->end,
        .data = _arena.create<expr::Ref>(expr::Ref{.expr = expr}),
    });
  } else if (peek().type == TOK_STAR) {
    Token star = consume();
    Expr *expr = try$(parse_expr(0, allow_struct_lit));
    return _arena.create<Expr>(Expr{
        .type = EXPR_DEREF,
        .start = star.start,
        .end = expr->end,
        .data = _arena.create<expr::Deref>(expr::Deref{.expr = expr}),
    });
  } else {
    return std::unexpected(
        Error("Expected a expression", peek().start, peek().end));
  }
}

ErrorOr<std::pair<std::vector<Param>, Type *>> Parser::parse_proc_signature() {
  Token oparen = try$(expect(TOK_OPAREN, "Expected `(`"));
  std::vector<Param> params{};
  while (_pos < _tokens.size() && peek().type != TOK_CPAREN) {
    std::vector<Token> names = try$(parse_name_list());
    Type *type = try$(parse_type());
    for (const Token &name: names)
      params.push_back(Param{name, type});

    if (peek().type == TOK_COMMA)
      consume();
    else
      break;
  }
  Token cparen = try$(expect(TOK_CPAREN, "Expected `)` after parameters"));
  Type *ret_type = try$(parse_type());
  return std::make_pair(std::move(params), ret_type);
}

ErrorOr<Type *> Parser::parse_type() {
  if (peek().type == TOK_OPAREN) {
    Token oparen = try$(expect(TOK_OPAREN, "Expected `(`"));
    if (peek().type == TOK_CPAREN) {
      return std::unexpected(
          Error("Expected type in parentheses", oparen.start, peek().end));
    }
    Type *first = try$(parse_type());
    if (peek().type == TOK_COMMA) {
      std::vector<Type *> elements{first};
      while (peek().type == TOK_COMMA) {
        consume();
        if (peek().type == TOK_CPAREN) {
          return std::unexpected(
              Error("Expected type after `,` in tuple type", peek().start,
                    peek().end));
        }
        elements.push_back(try$(parse_type()));
      }
      Token cparen = try$(expect(TOK_CPAREN, "Expected `)`"));
      return _arena.create<Type>(Type{
          .type = TYPE_TUPLE,
          .start = oparen.start,
          .end = cparen.end,
          .data = _arena.create<type::Tuple>(
              type::Tuple{.elements = elements}),
      });
    }
    return first;
  } else if (peek().type == TOK_PROC) {
    Token proc = try$(expect(TOK_PROC, "Expected `proc`"));
    auto sig = try$(parse_proc_signature());
    auto *proc_data = _arena.create<type::Proc>(type::Proc{
        .params = sig.first,
        .ret_type = sig.second,
    });
    return _arena.create<Type>(Type{
        .type = TYPE_PROC,
        .start = proc.start,
        .end = sig.second->end,
        .data = proc_data,
    });
  } else if (peek().type == TOK_ID) {
    if (peek().id_value == "bool") {
      Token bool_ = consume();
      return _arena.create<Type>(Type{
          .type = TYPE_BOOL,
          .start = bool_.start,
          .end = bool_.end,
      });
    } else if (peek().id_value == "int") {
      Token int_ = consume();
      return _arena.create<Type>(Type{
          .type = TYPE_INT,
          .start = int_.start,
          .end = int_.end,
      });
    } else if (peek().id_value == "void") {
      Token void_ = consume();
      return _arena.create<Type>(Type{
          .type = TYPE_VOID,
          .start = void_.start,
          .end = void_.end,
      });
    } else if (peek().id_value == "byte") {
      Token byte_ = consume();
      return _arena.create<Type>(Type{
          .type = TYPE_BYTE,
          .start = byte_.start,
          .end = byte_.end,
      });
    } else {
      Token name = consume();
      return _arena.create<Type>(Type{
          .type = TYPE_STRUCT,
          .start = name.start,
          .end = name.end,
          .data = _arena.create<type::Struct>(
              type::Struct{.name = name.id_value}),
      });
    }
  } else if (peek().type == TOK_STAR) {
    Token star = consume();
    Type *inner = try$(parse_type());
    return _arena.create<Type>(Type{
        .type = TYPE_PTR,
        .start = star.start,
        .end = inner->end,
        .data = _arena.create<type::Ptr>(type::Ptr{inner}),
    });
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
  return _arena.create<Stmt>(Stmt{
      .type = STMT_BLOCK,
      .start = obrace.start,
      .end = cbrace.end,
      .data = _arena.create<stmt::Block>(stmt::Block{block}),
  });
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

ErrorOr<std::vector<Token>> Parser::parse_name_list() {
  std::vector<Token> names{};
  names.push_back(try$(expect(TOK_ID, "Expected an identifier")));

  while (peek().type == TOK_COMMA) {
    consume();
    names.push_back(try$(expect(TOK_ID, "Expected an identifier")));
  }

  return names;
}
