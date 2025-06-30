#pragma once

#include <common.hpp>
#include <lexer/token.hpp>
#include <parser/ast.hpp>

class Parser {
public:
  Parser(const std::vector<Token> &tokens) : _tokens(tokens) {}

  ErrorOr<std::vector<Decl *>> parse_decls();

private:
  ErrorOr<Decl *> parse_decl();
  ErrorOr<Stmt *> parse_stmt();
  ErrorOr<Expr *> parse_expr(size_t max_prec = 100000);
  ErrorOr<Expr *> parse_primary_expr();
  ErrorOr<Type *> parse_type();

  ErrorOr<Stmt *> parse_block_stmt();
  ErrorOr<std::vector<Stmt *>> parse_block();

  ErrorOr<Token> expect(TokenType type, std::string message);
  Token consume();
  Token peek(size_t offset = 0);
  Token previous();

private:
  std::vector<Token> _tokens;
  size_t _pos = 0;
};
