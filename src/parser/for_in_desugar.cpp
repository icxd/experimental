#include <cstring>

#include <arena.hpp>
#include <common.hpp>
#include <parser/for_in_desugar.hpp>

namespace {

size_t g_for_in_tmp = 0;

Token synthetic_token(Arena *arena, std::string name, size_t start, size_t end) {
  char *storage = static_cast<char *>(arena->allocate(name.size() + 1));
  std::memcpy(storage, name.data(), name.size());
  storage[name.size()] = '\0';
  return Token{.type = TOK_ID,
               .start = start,
               .end = end,
               .id_value = std::string_view(storage, name.size())};
}

Expr *make_var_expr(Arena *arena, Token name) {
  return arena->create<Expr>(Expr{
      .type = EXPR_VAR,
      .start = name.start,
      .end = name.end,
      .data = arena->create<expr::Var>(expr::Var{.var = name}),
  });
}

Expr *make_int_expr(Arena *arena, int64_t value, size_t start, size_t end) {
  return arena->create<Expr>(Expr{
      .type = EXPR_INT,
      .start = start,
      .end = end,
      .data = arena->create<expr::Int>(expr::Int{.value = value}),
  });
}

Expr *make_index_expr(Arena *arena, Expr *base, int64_t idx, size_t start,
                      size_t end) {
  Expr *index = make_int_expr(arena, idx, start, end);
  return arena->create<Expr>(Expr{
      .type = EXPR_INDEX,
      .start = start,
      .end = end,
      .data = arena->create<expr::Index>(
          expr::Index{.base = base, .index = index}),
  });
}

Expr *make_method_call(Arena *arena, Expr *receiver, std::string_view method) {
  Token method_token =
      synthetic_token(arena, std::string(method), receiver->end, receiver->end);
  return arena->create<Expr>(Expr{
      .type = EXPR_CALL,
      .start = receiver->start,
      .end = receiver->end,
      .data = arena->create<expr::Call>(
          expr::Call{.name = method_token, .arguments = {}, .receiver = receiver}),
  });
}

Stmt *make_var_stmt(Arena *arena, size_t start, size_t end, Token name,
                    Type *type, Expr *value) {
  return arena->create<Stmt>(Stmt{
      .type = STMT_VAR,
      .start = start,
      .end = end,
      .data = arena->create<stmt::Var>(
          stmt::Var{.name = name, .type = type, .value = value}),
  });
}

Stmt *make_assign_stmt(Arena *arena, size_t start, size_t end, Expr *target,
                       Expr *value) {
  return arena->create<Stmt>(Stmt{
      .type = STMT_ASSIGN,
      .start = start,
      .end = end,
      .data = arena->create<stmt::Assign>(
          stmt::Assign{.target = target, .value = value}),
  });
}

Stmt *make_while_stmt(Arena *arena, size_t start, size_t end, Expr *cond,
                      std::vector<Stmt *> body) {
  return arena->create<Stmt>(Stmt{
      .type = STMT_WHILE,
      .start = start,
      .end = end,
      .data = arena->create<stmt::While>(stmt::While{.cond = cond, .body = body}),
  });
}

ErrorOr<std::vector<Stmt *>>
make_tuple_destructure(Arena *arena, size_t start, size_t end,
                       std::vector<Token> names, Expr *rhs, bool declare_names) {
  if (names.empty()) {
    return std::unexpected(
        Error("Expected at least one name in destructure pattern", start, end));
  }

  Token tmp =
      synthetic_token(arena, std::format("__dtmp{}", g_for_in_tmp++), start,
                      rhs->end);
  std::vector<Stmt *> stmts{};
  stmts.push_back(make_var_stmt(arena, start, end, tmp, nullptr, rhs));

  Expr *base = make_var_expr(arena, tmp);
  for (size_t i = 0; i < names.size(); i++) {
    if (names[i].id_value == "_")
      continue;
    Expr *elem =
        make_index_expr(arena, base, static_cast<int64_t>(i), names[i].start,
                        names[i].end);
    if (declare_names) {
      stmts.push_back(make_var_stmt(arena, names[i].start, names[i].end,
                                    names[i], nullptr, elem));
    } else {
      stmts.push_back(make_assign_stmt(arena, names[i].start, names[i].end,
                                       make_var_expr(arena, names[i]), elem));
    }
  }

  if (stmts.size() == 1) {
    return std::unexpected(Error(
        "Destructure pattern must bind at least one name", start, end));
  }

  return stmts;
}

} // namespace

ErrorOr<std::vector<Stmt *>> desugar_for_in(stmt::ForIn *for_in, size_t end,
                                            Arena *arena) {
  if (arena == nullptr)
    PANIC("arena required for for-in desugar");

  size_t anchor = for_in->iterable->end;
  size_t suffix = g_for_in_tmp++;
  Token it_token =
      synthetic_token(arena, std::format("__rye_it{}", suffix), anchor, anchor);
  Token ok_token =
      synthetic_token(arena, std::format("__rye_ok{}", suffix), anchor, anchor);
  Token binding_token = for_in->binding->name;

  Expr *iter_call = make_method_call(arena, for_in->iterable, "iter");
  Stmt *it_decl =
      make_var_stmt(arena, anchor, anchor, it_token, nullptr, iter_call);

  Expr *it_expr = make_var_expr(arena, it_token);
  Expr *next_call = make_method_call(arena, it_expr, "next");

  std::vector<Stmt *> init_stmts{};
  if (for_in->binding->type != nullptr) {
    Token tmp =
        synthetic_token(arena, std::format("__rye_next{}", suffix), anchor,
                        anchor);
    init_stmts.push_back(
        make_var_stmt(arena, anchor, anchor, tmp, nullptr, next_call));
    Expr *tmp_expr = make_var_expr(arena, tmp);
    init_stmts.push_back(
        make_var_stmt(arena, anchor, anchor, ok_token, nullptr,
                      make_index_expr(arena, tmp_expr, 0, anchor, anchor)));
    init_stmts.push_back(
        make_var_stmt(arena, binding_token.start, binding_token.end,
                      binding_token, for_in->binding->type,
                      make_index_expr(arena, tmp_expr, 1, binding_token.start,
                                      binding_token.end)));
  } else {
    std::vector<Stmt *> destructure = try$(
        make_tuple_destructure(arena, anchor, anchor, {ok_token, binding_token},
                               next_call, true));
    init_stmts.insert(init_stmts.end(), destructure.begin(), destructure.end());
  }

  std::vector<Stmt *> while_body = for_in->body;
  Expr *next_step = make_method_call(arena, make_var_expr(arena, it_token), "next");
  std::vector<Stmt *> step_stmts = try$(
      make_tuple_destructure(arena, anchor, anchor, {ok_token, binding_token},
                             next_step, false));
  while_body.insert(while_body.end(), step_stmts.begin(), step_stmts.end());

  Stmt *while_stmt =
      make_while_stmt(arena, anchor, end, make_var_expr(arena, ok_token),
                      while_body);

  std::vector<Stmt *> out{it_decl};
  out.insert(out.end(), init_stmts.begin(), init_stmts.end());
  out.push_back(while_stmt);
  return out;
}

ErrorOr<void> expand_for_in_stmts(std::vector<Stmt *> &stmts, Arena *arena) {
  for (size_t i = 0; i < stmts.size();) {
    Stmt *stmt = stmts[i];

    if (stmt->type == STMT_FOR_IN) {
      auto *for_in = std::get<stmt::ForIn *>(stmt->data);
      std::vector<Stmt *> expanded =
          try$(desugar_for_in(for_in, stmt->end, arena));
      stmts.erase(stmts.begin() + static_cast<std::ptrdiff_t>(i));
      stmts.insert(stmts.begin() + static_cast<std::ptrdiff_t>(i),
                   expanded.begin(), expanded.end());
      i += expanded.size();
      continue;
    }

    if (stmt->type == STMT_BLOCK) {
      auto *block = std::get<stmt::Block *>(stmt->data);
      try$(expand_for_in_stmts(block->stmts, arena));
    } else if (stmt->type == STMT_IF) {
      auto *iff = std::get<stmt::If *>(stmt->data);
      try$(expand_for_in_stmts(iff->then_block, arena));
      try$(expand_for_in_stmts(iff->else_block, arena));
    } else if (stmt->type == STMT_WHILE) {
      auto *loop = std::get<stmt::While *>(stmt->data);
      try$(expand_for_in_stmts(loop->body, arena));
    } else if (stmt->type == STMT_FOR) {
      auto *loop = std::get<stmt::For *>(stmt->data);
      try$(expand_for_in_stmts(loop->body, arena));
    }

    ++i;
  }

  return {};
}
