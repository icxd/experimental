#pragma once

#include <arena.hpp>
#include <common.hpp>
#include <lexer/token.hpp>
#include <parser/ast.hpp>

class Parser {
public:
  Parser(const std::vector<Token> &tokens, Arena &arena) :
      _tokens(tokens), _arena(arena) {}

  ErrorOr<std::vector<Decl *>> parse_decls();

private:
  ErrorOr<Decl *> parse_decl();
  ErrorOr<Stmt *> parse_stmt();
  ErrorOr<Expr *> parse_expr(size_t min_prec = 0, bool allow_struct_lit = true);
  ErrorOr<Expr *> parse_postfix_expr(bool allow_struct_lit = true);
  ErrorOr<Expr *> parse_primary_expr(bool allow_struct_lit = true);
  ErrorOr<Type *> parse_type();
  ErrorOr<std::pair<std::vector<Param>, Type *>> parse_proc_signature();
  ErrorOr<std::vector<Token>> parse_name_list();

  ErrorOr<Stmt *> parse_block_stmt();
  ErrorOr<std::vector<Stmt *>> parse_block();
  ErrorOr<Stmt *> parse_var_destructure(Token var, Token oparen);
  Stmt *make_var_stmt(size_t start, size_t end, Token name, Type *type,
                      std::optional<Expr *> value);
  Token synthetic_id(std::string name, size_t start, size_t end);
  Expr *make_var_expr(Token name);
  Expr *make_int_expr(int64_t value, size_t start, size_t end);
  Expr *make_index_expr(Expr *base, int64_t idx, size_t start, size_t end);

  ErrorOr<Token> expect(TokenType type, std::string message);
  Token consume();
  Token peek(size_t offset = 0);
  Token previous();

private:
  std::vector<Token> _tokens;
  size_t _pos = 0;
  Arena &_arena;
  size_t _destructure_tmp = 0;
};
