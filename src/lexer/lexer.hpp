#pragma once

#include <common.hpp>
#include <lexer/token.hpp>

class Lexer {
public:
private:
  std::string _source;
  size_t _pos = 0;
};
