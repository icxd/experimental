#pragma once

#include <utility>

#include <common.hpp>

enum TokenType {
  TOK_ID,
  TOK_INT,

  TOK_CONST,
  TOK_ELSE,
  TOK_IF,
  TOK_FOR,
  TOK_WHILE,
  TOK_EXTERN,
  TOK_PROC,
  TOK_RETURN,
  TOK_VAR,
  TOK_WHEN,
  TOK_BREAK,
  TOK_CONTINUE,

  TOK_OPAREN,
  TOK_CPAREN,
  TOK_OBRACE,
  TOK_CBRACE,
  TOK_COMMA,
  TOK_SEMICOLON,

  TOK_PLUS,
  TOK_MINUS,
  TOK_STAR,
  TOK_SLASH,
  TOK_EQ,
  TOK_EQEQ,
  TOK_BANG,
  TOK_NEQ,
  TOK_LT,
  TOK_LTE,
  TOK_GT,
  TOK_GTE,
  TOK_AMPERSAND,
  TOK_AMPAMP,
  TOK_PIPEPIPE,

  TOK_EOF,
};

struct Token {
  TokenType type;
  size_t start, end;

  union {
    std::string_view id_value;
    int64_t int_value;
  };

  std::string to_string() const {
    switch (type) {
    case TOK_ID:        return std::format("TOK_ID({})", id_value);
    case TOK_INT:       return std::format("TOK_INT({})", int_value);
    case TOK_CONST:     return std::format("TOK_CONST");
    case TOK_ELSE:      return std::format("TOK_ELSE");
    case TOK_EXTERN:    return std::format("TOK_EXTERN");
    case TOK_PROC:      return std::format("TOK_PROC");
    case TOK_RETURN:    return std::format("TOK_RETURN");
    case TOK_VAR:       return std::format("TOK_VAR");
    case TOK_WHEN:      return std::format("TOK_WHEN");
    case TOK_BREAK:     return std::format("TOK_BREAK");
    case TOK_CONTINUE:  return std::format("TOK_CONTINUE");
    case TOK_OPAREN:    return std::format("TOK_OPAREN");
    case TOK_CPAREN:    return std::format("TOK_CPAREN");
    case TOK_OBRACE:    return std::format("TOK_OBRACE");
    case TOK_CBRACE:    return std::format("TOK_CBRACE");
    case TOK_COMMA:     return std::format("TOK_COMMA");
    case TOK_SEMICOLON: return std::format("TOK_SEMICOLON");
    case TOK_PLUS:      return std::format("TOK_PLUS");
    case TOK_IF:        return std::format("TOK_IF");
    case TOK_FOR:       return std::format("TOK_FOR");
    case TOK_WHILE:     return std::format("TOK_WHILE");
    case TOK_MINUS:     return std::format("TOK_MINUS");
    case TOK_STAR:      return std::format("TOK_STAR");
    case TOK_SLASH:     return std::format("TOK_SLASH");
    case TOK_EQ:        return std::format("TOK_EQ");
    case TOK_EQEQ:      return std::format("TOK_EQEQ");
    case TOK_BANG:      return std::format("TOK_BANG");
    case TOK_NEQ:       return std::format("TOK_NEQ");
    case TOK_LT:        return std::format("TOK_LT");
    case TOK_LTE:       return std::format("TOK_LTE");
    case TOK_GT:        return std::format("TOK_GT");
    case TOK_GTE:       return std::format("TOK_GTE");
    case TOK_AMPERSAND: return std::format("TOK_AMPERSAND");
    case TOK_AMPAMP:    return std::format("TOK_AMPAMP");
    case TOK_PIPEPIPE:  return std::format("TOK_PIPEPIPE");
    case TOK_EOF:       return std::format("TOK_EOF");
    }
    std::unreachable();
  }
};
