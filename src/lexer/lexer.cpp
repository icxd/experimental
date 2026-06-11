#include <lexer/lexer.hpp>
#include <lexer/token.hpp>

ErrorOr<std::vector<Token>> Lexer::lex_tokens() {
  std::vector<Token> tokens{};
  while (_pos < _source.size()) {
    if (_pos + 1 < _source.size() && peek() == '/' && peek(1) == '/') {
      _pos += 2;
      while (_pos < _source.size() && peek() != '\n')
        _pos++;
      _pos++;
      continue;
    }

    auto token = next_token();
    if (!token.has_value())
      return std::unexpected(token.error());
    tokens.push_back(token.value());
  }
  return tokens;
}

ErrorOr<Token> Lexer::next_token() {
  skip_whitespace();
  if (_pos >= _source.size())
    return Token{.type = TOK_EOF, .start = _pos, .end = _pos};

  if (std::isalpha(peek()) || peek() == '_') {
    size_t start = _pos;
    while (_pos < _source.size() && (std::isalnum(peek()) || peek() == '_'))
      _pos++;
    std::string_view id = _source.substr(start, _pos - start);

    TokenType type = TOK_ID;
    if (id == "const")
      type = TOK_CONST;
    else if (id == "comptime")
      type = TOK_COMPTIME;
    else if (id == "else")
      type = TOK_ELSE;
    else if (id == "if")
      type = TOK_IF;
    else if (id == "for")
      type = TOK_FOR;
    else if (id == "while")
      type = TOK_WHILE;
    else if (id == "extern")
      type = TOK_EXTERN;
    else if (id == "proc")
      type = TOK_PROC;
    else if (id == "var")
      type = TOK_VAR;
    else if (id == "return")
      type = TOK_RETURN;
    else if (id == "when")
      type = TOK_WHEN;
    else if (id == "break")
      type = TOK_BREAK;
    else if (id == "continue")
      type = TOK_CONTINUE;
    else if (id == "import")
      type = TOK_IMPORT;

    return Token{
        .type = type,
        .start = start,
        .end = _pos,
        .id_value = id,
    };
  }

  if (peek() == '"') {
    size_t start = _pos;
    _pos++;
    while (_pos < _source.size() && peek() != '"') {
      if (peek() == '\\' && _pos + 1 < _source.size())
        _pos += 2;
      else
        _pos++;
    }
    if (_pos >= _source.size()) {
      return std::unexpected(
          Error("Unterminated string literal", start, _source.size()));
    }
    std::string_view value = _source.substr(start + 1, _pos - start - 1);
    _pos++;
    return Token{.type = TOK_STRING,
                 .start = start,
                 .end = _pos,
                 .string_value = value};
  }

  if (std::isdigit(peek())) {
    size_t start = _pos;
    while (_pos < _source.size() && std::isdigit(peek()))
      _pos++;
    std::string s = std::string(_source.substr(start, _pos - start));
    int64_t int_value = std::stoll(s);
    return Token{
        .type = TOK_INT,
        .start = start,
        .end = _pos,
        .int_value = int_value,
    };
  }

  switch (peek()) {
  case '(': return single_char_token(TOK_OPAREN);
  case ')': return single_char_token(TOK_CPAREN);
  case '{': return single_char_token(TOK_OBRACE);
  case '}': return single_char_token(TOK_CBRACE);
  case ',': return single_char_token(TOK_COMMA);
  case ';': return single_char_token(TOK_SEMICOLON);
  case '+': return single_char_token(TOK_PLUS);
  case '-': return single_char_token(TOK_MINUS);
  case '*': return single_char_token(TOK_STAR);
  case '/': return single_char_token(TOK_SLASH);
  case '=': return double_char_token('=', TOK_EQEQ, TOK_EQ);
  case '!': return double_char_token('=', TOK_NEQ, TOK_BANG);
  case '<': return double_char_token('=', TOK_LTE, TOK_LT);
  case '>': return double_char_token('=', TOK_GTE, TOK_GT);
  case ':': return single_char_token(TOK_COLON);
  case '&': return double_char_token('&', TOK_AMPAMP, TOK_AMPERSAND);
  case '|':
    if (_pos + 1 < _source.size() && peek(1) == '|')
      return double_char_token('|', TOK_PIPEPIPE, TOK_PIPEPIPE);
    return std::unexpected(
        Error(std::format("Illegal character `{}`", peek()), _pos, _pos + 1));
  default:  break;
  }

  return std::unexpected(
      Error(std::format("Illegal character `{}`", peek()), _pos, _pos + 1));
}

void Lexer::skip_whitespace() {
  while (_pos < _source.size() && std::isspace(peek()))
    _pos++;
}

char Lexer::consume() {
  char ch = _source.at(_pos);
  _pos++;
  return ch;
}

char Lexer::peek(size_t offset) { return _source.at(_pos + offset); }

Token Lexer::single_char_token(TokenType type) {
  Token token{.type = type, .start = _pos, .end = _pos + 1};
  _pos++;
  return token;
}

Token Lexer::double_char_token(char followup, TokenType first_token,
                               TokenType second_token) {
  if (_pos + 1 < _source.size() && peek(1) == followup) {
    Token token{first_token, _pos, _pos + 2};
    _pos += 2;
    return token;
  }

  return single_char_token(second_token);
}
