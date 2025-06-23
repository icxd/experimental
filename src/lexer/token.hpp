#pragma once

#include <common.hpp>

enum TokenType {
  TOK_ID,
  TOK_INT,

  TOK_PROC,
  TOK_VAR,
  TOK_RETURN,

  TOK_OPAREN,
  TOK_CPAREN,
  TOK_OBRACE,
  TOK_CBRACE,

  TOK_STAR,
  TOK_EQ,
  TOK_AMPERSAND,
};

struct Token {
  TokenType type;
  size_t start, end;

  union {
    std::string_view id_value;
    int64_t int_value;
  };
};
