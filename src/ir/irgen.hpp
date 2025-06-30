#pragma once

#include <checker/checker.hpp>
#include <common.hpp>
#include <ir/ir.hpp>
#include <parser/ast.hpp>

class Generator {
public:
  Generator(const std::vector<Decl *> &decls) : _decls(decls) {}

  void gen_decls();

  Builder builder() const { return _builder; }

private:
  void gen_decl(Decl *decl);
  void gen_stmt(Stmt *stmt, Function *fn);
  Operand gen_expr(Expr *expr, Function *fn);

private:
  std::vector<Decl *> _decls;
  Builder _builder{};
};
