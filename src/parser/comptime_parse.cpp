#include <cstring>

#include <parser/comptime_parse.hpp>

#include <lexer/lexer.hpp>
#include <parser/parser.hpp>

ErrorOr<Stmt *> parse_single_stmt(Arena &arena, std::string_view source) {
  char *storage = static_cast<char *>(arena.allocate(source.size() + 1));
  std::memcpy(storage, source.data(), source.size());
  storage[source.size()] = '\0';
  std::string_view stable_source(storage, source.size());

  Lexer lexer(stable_source);
  auto tokens = lexer.lex_tokens();
  if (!tokens.has_value())
    return std::unexpected(tokens.error());

  Parser parser(tokens.value(), arena);
  auto stmt = parser.parse_stmt();
  if (!stmt.has_value())
    return std::unexpected(stmt.error());

  if (!parser.at_end()) {
    return std::unexpected(
        Error("Expected a single statement", 0, 0)
            .with_hint("Remove extra tokens after the statement"));
  }

  return stmt.value();
}

ErrorOr<Decl *> parse_single_decl(Arena &arena, std::string_view source) {
  char *storage = static_cast<char *>(arena.allocate(source.size() + 1));
  std::memcpy(storage, source.data(), source.size());
  storage[source.size()] = '\0';
  std::string_view stable_source(storage, source.size());

  Lexer lexer(stable_source);
  auto tokens = lexer.lex_tokens();
  if (!tokens.has_value())
    return std::unexpected(tokens.error());

  Parser parser(tokens.value(), arena);
  auto decl = parser.parse_decl();
  if (!decl.has_value())
    return std::unexpected(decl.error());

  if (!parser.at_end()) {
    return std::unexpected(
        Error("Expected a single declaration", 0, 0)
            .with_hint("Remove extra tokens after the declaration"));
  }

  return decl.value();
}
