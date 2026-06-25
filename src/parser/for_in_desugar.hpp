#pragma once

#include <vector>

#include <checker/checker_types.hpp>
#include <common.hpp>
#include <parser/ast.hpp>

class Arena;

ErrorOr<std::vector<Stmt *>> desugar_for_in(stmt::ForIn *for_in, size_t end,
                                            Arena *arena);
ErrorOr<void> expand_for_in_stmts(std::vector<Stmt *> &stmts, Arena *arena);
