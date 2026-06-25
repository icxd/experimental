#pragma once

#include <string_view>

#include <arena.hpp>
#include <common.hpp>
#include <parser/ast.hpp>

ErrorOr<Stmt *> parse_single_stmt(Arena &arena, std::string_view source);
ErrorOr<Decl *> parse_single_decl(Arena &arena, std::string_view source);
