#pragma once

#include <common.hpp>
#include <lexer/token.hpp>

class Lexer {
public:
  explicit Lexer(std::string_view source) : _source(std::move(source)) {}

  [[nodiscard]] ErrorOr<std::vector<Token>> lex_tokens();
  [[nodiscard]] ErrorOr<Token> next_token();

private:
  void skip_whitespace();
  [[nodiscard]] char consume();
  [[nodiscard]] char peek(size_t offset = 0);

  Token single_char_token(TokenType type);
  Token double_char_token(char followup, TokenType first_token,
                          TokenType second_token);

private:
  std::string_view _source;
  size_t _pos = 0;
};
