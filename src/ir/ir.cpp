#include <algorithm>
#include <functional>
#include <unordered_map>
#include <unordered_set>

#include <codegen/regalloc.hpp>
#include <ir/ir.hpp>

Function *Builder::create_function(std::string_view name,
                                   std::vector<std::string_view> parameters) {
  Function *fn = new Function{name, parameters};
  _functions.push_back(fn);
  return fn;
}

void Builder::print() const {
  for (const auto &fn: _functions)
    fn->print();
}

Operand Builder::new_temp() {
  std::string name = std::format("t{}", _temp_counter++);
  return Operand::Temporary(name);
}

static bool operands_equal(const Operand &a, const Operand &b) {
  if (a.type != b.type)
    return false;
  if (a.type == OPERAND_CONSTANT_INT)
    return a.int_value == b.int_value;
  return a.name == b.name;
}

static Operand resolve_copy(const Operand &op,
                            const std::unordered_map<std::string, Operand> &copies) {
  if (op.type != OPERAND_TEMPORARY)
    return op;
  auto it = copies.find(op.name);
  if (it == copies.end())
    return op;
  return resolve_copy(it->second, copies);
}

static void substitute_copies(
    Instruction &instr, const std::unordered_map<std::string, Operand> &copies) {
  for (Operand &src: instr.srcs)
    src = resolve_copy(src, copies);
}

static std::optional<int64_t>
known_operand_value(const Operand &op,
                    const std::unordered_map<std::string, int64_t> &temps) {
  if (op.type == OPERAND_CONSTANT_INT)
    return op.int_value;
  if (op.type == OPERAND_TEMPORARY) {
    auto it = temps.find(op.name);
    if (it != temps.end())
      return it->second;
  }
  return std::nullopt;
}

static void record_temp_constant(std::unordered_map<std::string, int64_t> &temps,
                                 const Instruction &instr) {
  if (instr.dst.has_value() && instr.dst->type == OPERAND_TEMPORARY)
    temps.erase(instr.dst->name);

  if (instr.opcode == OP_ASSIGN && instr.dst.has_value() &&
      instr.dst->type == OPERAND_TEMPORARY && instr.srcs.size() == 1 &&
      instr.srcs[0].type == OPERAND_CONSTANT_INT)
    temps[instr.dst->name] = instr.srcs[0].int_value;
}

static bool is_pure_opcode(Opcode op) {
  switch (op) {
  case OP_NOP:
  case OP_ASSIGN:
  case OP_ADDROF:
  case OP_DEREF:
  case OP_LOAD_OFFSET:
  case OP_LOAD_LABEL:
  case OP_ADD:
  case OP_SUB:
  case OP_MUL:
  case OP_DIV:
  case OP_CMP_EQ:
  case OP_CMP_NEQ:
  case OP_CMP_LT:
  case OP_CMP_LTE:
  case OP_CMP_GT:
  case OP_CMP_GTE:
    return true;
  default:
    return false;
  }
}

static std::string operand_key(const Operand &op) {
  switch (op.type) {
  case OPERAND_VARIABLE:
  case OPERAND_STACK_ADDR:
  case OPERAND_TEMPORARY:
  case OPERAND_CONSTANT:
  case OPERAND_FUNCTION:
  case OPERAND_LABEL:
    return std::format("{}:{}", static_cast<int>(op.type), op.name);
  case OPERAND_CONSTANT_INT:
    return std::format("i:{}", op.int_value);
  }
  std::unreachable();
}

struct LoadKey {
  std::string base;
  int64_t offset = 0;

  bool operator==(const LoadKey &other) const {
    return base == other.base && offset == other.offset;
  }
};

struct LoadKeyHash {
  size_t operator()(const LoadKey &key) const {
    return std::hash<std::string>{}(key.base) ^
           (std::hash<int64_t>{}(key.offset) << 1);
  }
};

struct ExprKey {
  Opcode opcode = OP_NOP;
  std::string lhs;
  std::string rhs;

  bool operator==(const ExprKey &other) const {
    return opcode == other.opcode && lhs == other.lhs && rhs == other.rhs;
  }
};

struct ExprKeyHash {
  size_t operator()(const ExprKey &key) const {
    size_t h = std::hash<int>{}(static_cast<int>(key.opcode));
    h ^= std::hash<std::string>{}(key.lhs) << 1;
    h ^= std::hash<std::string>{}(key.rhs) << 2;
    return h;
  }
};

static LoadKey load_key(const Operand &base, int64_t offset) {
  return LoadKey{operand_key(base), offset};
}

template <typename Map>
static void erase_load_keys_for_base(Map &map, std::string_view base_key) {
  std::vector<LoadKey> to_erase;
  for (const auto &[key, _]: map) {
    if (key.base == base_key)
      to_erase.push_back(key);
  }
  for (const LoadKey &key: to_erase)
    map.erase(key);
}

static int count_temp_uses(const std::vector<Instruction> &input,
                             std::string_view temp) {
  int uses = 0;
  for (const Instruction &instr: input) {
    for (const Operand &src: instr.srcs) {
      if (src.type == OPERAND_TEMPORARY && src.name == temp)
        uses++;
    }
  }
  return uses;
}

static bool is_commutative(Opcode op) {
  switch (op) {
  case OP_ADD:
  case OP_MUL:
  case OP_CMP_EQ:
  case OP_CMP_NEQ:
    return true;
  default:
    return false;
  }
}

static ExprKey expr_key(Opcode opcode, const Operand &lhs, const Operand &rhs) {
  std::string a = operand_key(lhs);
  std::string b = operand_key(rhs);
  if (is_commutative(opcode) && b < a)
    std::swap(a, b);
  return ExprKey{opcode, a, b};
}

std::vector<Instruction> fold_constants(const std::vector<Instruction> &input) {
  std::unordered_map<std::string, int64_t> temps{};
  std::vector<Instruction> output{};
  output.reserve(input.size());

  for (const Instruction &instr: input) {
    Instruction folded = instr;

    if (instr.dst.has_value() && instr.dst->type == OPERAND_TEMPORARY)
      temps.erase(instr.dst->name);

    auto assign_dst_src = [&](const Operand &src) {
      folded.opcode = OP_ASSIGN;
      folded.srcs = {src};
    };

    auto fold_binary = [&](auto fn) -> bool {
      if (!instr.dst.has_value() || instr.srcs.size() != 2)
        return false;
      auto lhs = known_operand_value(instr.srcs[0], temps);
      auto rhs = known_operand_value(instr.srcs[1], temps);
      if (!lhs.has_value() || !rhs.has_value())
        return false;
      assign_dst_src(Operand::ConstantInt(fn(*lhs, *rhs)));
      return true;
    };

    switch (instr.opcode) {
    case OP_ADD:
      if (known_operand_value(instr.srcs[1], temps) == 0)
        assign_dst_src(instr.srcs[0]);
      else if (known_operand_value(instr.srcs[0], temps) == 0)
        assign_dst_src(instr.srcs[1]);
      else
        fold_binary(std::plus<>());
      break;
    case OP_SUB:
      if (known_operand_value(instr.srcs[1], temps) == 0)
        assign_dst_src(instr.srcs[0]);
      else
        fold_binary(std::minus<>());
      break;
    case OP_MUL:
      if (known_operand_value(instr.srcs[1], temps) == 1)
        assign_dst_src(instr.srcs[0]);
      else if (known_operand_value(instr.srcs[0], temps) == 1)
        assign_dst_src(instr.srcs[1]);
      else if (known_operand_value(instr.srcs[1], temps) == 0 ||
               known_operand_value(instr.srcs[0], temps) == 0)
        assign_dst_src(Operand::ConstantInt(0));
      else
        fold_binary(std::multiplies<>());
      break;
    case OP_DIV:
      if (known_operand_value(instr.srcs[1], temps) == 1)
        assign_dst_src(instr.srcs[0]);
      else
        fold_binary(std::divides<>());
      break;
    case OP_CMP_EQ:
      if (operands_equal(instr.srcs[0], instr.srcs[1]))
        assign_dst_src(Operand::ConstantInt(1));
      else
        fold_binary([](int64_t a, int64_t b) { return a == b ? 1 : 0; });
      break;
    case OP_CMP_NEQ:
      if (operands_equal(instr.srcs[0], instr.srcs[1]))
        assign_dst_src(Operand::ConstantInt(0));
      else
        fold_binary([](int64_t a, int64_t b) { return a != b ? 1 : 0; });
      break;
    case OP_CMP_LT:
      fold_binary([](int64_t a, int64_t b) { return a < b ? 1 : 0; });
      break;
    case OP_CMP_LTE:
      fold_binary([](int64_t a, int64_t b) { return a <= b ? 1 : 0; });
      break;
    case OP_CMP_GT:
      fold_binary([](int64_t a, int64_t b) { return a > b ? 1 : 0; });
      break;
    case OP_CMP_GTE:
      fold_binary([](int64_t a, int64_t b) { return a >= b ? 1 : 0; });
      break;
    default:
      break;
    }

    record_temp_constant(temps, folded);
    output.push_back(folded);
  }

  return output;
}

std::vector<Instruction>
copy_propagate(const std::vector<Instruction> &input) {
  std::unordered_map<std::string, Operand> copies{};
  std::vector<Instruction> output{};
  output.reserve(input.size());

  for (Instruction instr: input) {
    if (instr.opcode == OP_LABEL)
      copies.clear();

    substitute_copies(instr, copies);

    if (instr.dst.has_value() && instr.dst->type == OPERAND_TEMPORARY)
      copies.erase(instr.dst->name);

    if (instr.opcode == OP_ASSIGN && instr.dst.has_value() &&
        instr.dst->type == OPERAND_TEMPORARY && instr.srcs.size() == 1) {
      const Operand &src = instr.srcs[0];
      if (src.type == OPERAND_VARIABLE || src.type == OPERAND_STACK_ADDR ||
          src.type == OPERAND_CONSTANT_INT || src.type == OPERAND_CONSTANT ||
          src.type == OPERAND_TEMPORARY)
        copies[instr.dst->name] = src;
    }

    if (instr.opcode == OP_STORE_OFFSET && instr.srcs.size() == 3) {
      std::string base_key = operand_key(instr.srcs[0]);
      for (auto it = copies.begin(); it != copies.end();) {
        if (it->second.type == OPERAND_VARIABLE &&
            operand_key(it->second) == base_key)
          it = copies.erase(it);
        else
          ++it;
      }
    }

    output.push_back(instr);
  }

  return output;
}

std::vector<Instruction>
fold_branches(const std::vector<Instruction> &input) {
  std::unordered_map<std::string, int64_t> temps{};
  std::vector<Instruction> output{};
  output.reserve(input.size());

  for (const Instruction &instr: input) {
    if (instr.dst.has_value() && instr.dst->type == OPERAND_TEMPORARY)
      temps.erase(instr.dst->name);

    record_temp_constant(temps, instr);

    if (instr.opcode == OP_JMP_IF_ZERO && instr.srcs.size() == 2) {
      auto cond = known_operand_value(instr.srcs[0], temps);
      if (cond.has_value()) {
        if (*cond == 0) {
          Instruction jmp;
          jmp.opcode = OP_JMP;
          jmp.srcs = {instr.srcs[1]};
          output.push_back(jmp);
        }
        continue;
      }
    }

    if (instr.opcode == OP_JMP_IF_NONZERO && instr.srcs.size() == 2) {
      auto cond = known_operand_value(instr.srcs[0], temps);
      if (cond.has_value()) {
        if (*cond != 0) {
          Instruction jmp;
          jmp.opcode = OP_JMP;
          jmp.srcs = {instr.srcs[1]};
          output.push_back(jmp);
        }
        continue;
      }
    }

    output.push_back(instr);
  }

  return output;
}

static bool is_cmp_opcode(Opcode op) {
  return op >= OP_CMP_EQ && op <= OP_CMP_GTE;
}

static std::optional<Operand> materialize_call_arg(const Instruction &def_instr) {
  if (def_instr.opcode == OP_ADDROF && def_instr.srcs.size() == 1 &&
      def_instr.srcs[0].type == OPERAND_VARIABLE)
    return Operand::StackAddr(def_instr.srcs[0].name);
  if (def_instr.opcode == OP_LOAD_LABEL && def_instr.srcs.size() == 1)
    return def_instr.srcs[0];
  if (def_instr.opcode == OP_ASSIGN && def_instr.srcs.size() == 1 &&
      def_instr.srcs[0].type != OPERAND_TEMPORARY)
    return def_instr.srcs[0];
  return std::nullopt;
}

static std::optional<Operand>
folded_call_arg(const Instruction &def_instr) {
  switch (def_instr.opcode) {
  case OP_ADDROF:
    return materialize_call_arg(def_instr);
  case OP_ASSIGN:
  case OP_LOAD_OFFSET:
  case OP_LOAD_LABEL:
    if (def_instr.srcs.size() == 1)
      return def_instr.srcs[0];
    return std::nullopt;
  case OP_ADD:
  case OP_SUB:
  case OP_MUL:
  case OP_DIV:
  case OP_CMP_EQ:
  case OP_CMP_NEQ:
  case OP_CMP_LT:
  case OP_CMP_LTE:
  case OP_CMP_GT:
  case OP_CMP_GTE:
    return std::nullopt;
  default:
    return std::nullopt;
  }
}

static bool try_eliminate_single_use_temp(const Instruction &def_instr,
                                          const Instruction &use_instr,
                                          std::string_view temp_name,
                                          Instruction &replacement) {
  if (!def_instr.dst || def_instr.dst->name != temp_name)
    return false;

  if (use_instr.opcode == OP_RET && use_instr.srcs.size() == 1 &&
      use_instr.srcs[0].type == OPERAND_TEMPORARY &&
      use_instr.srcs[0].name == temp_name) {
    if (is_cmp_opcode(def_instr.opcode) && def_instr.srcs.size() == 2) {
      replacement.opcode = OP_RET;
      replacement.dst = std::nullopt;
      replacement.srcs = {Operand::ConstantInt(
                              static_cast<int64_t>(def_instr.opcode)),
                            def_instr.srcs[0], def_instr.srcs[1]};
      return true;
    }
    return false;
  }

  if (use_instr.opcode == OP_CALL && use_instr.srcs.size() >= 1) {
    for (size_t i = 1; i < use_instr.srcs.size(); i++) {
      if (use_instr.srcs[i].type != OPERAND_TEMPORARY ||
          use_instr.srcs[i].name != temp_name)
        continue;
      auto arg = folded_call_arg(def_instr);
      if (!arg.has_value())
        return false;
      replacement = use_instr;
      replacement.srcs[i] = *arg;
      return true;
    }
    return false;
  }

  if (use_instr.opcode != OP_ASSIGN || use_instr.srcs.size() != 1 ||
      use_instr.srcs[0].type != OPERAND_TEMPORARY ||
      use_instr.srcs[0].name != temp_name || !use_instr.dst)
    return false;

  switch (def_instr.opcode) {
  case OP_ADDROF:
  case OP_DEREF:
    if (def_instr.srcs.size() != 1)
      return false;
    replacement.opcode = def_instr.opcode;
    replacement.dst = use_instr.dst;
    replacement.srcs = def_instr.srcs;
    return true;

  case OP_ASSIGN:
  case OP_LOAD_OFFSET:
  case OP_LOAD_LABEL:
    if (def_instr.srcs.empty())
      return false;
    replacement.opcode = def_instr.opcode;
    replacement.dst = use_instr.dst;
    replacement.srcs = def_instr.srcs;
    return true;

  case OP_ADD:
  case OP_SUB:
  case OP_MUL:
  case OP_DIV:
    if (def_instr.srcs.size() != 2)
      return false;
    replacement.opcode = def_instr.opcode;
    replacement.dst = use_instr.dst;
    replacement.srcs = def_instr.srcs;
    return true;

  case OP_CALL:
    if (def_instr.srcs.empty())
      return false;
    replacement.opcode = def_instr.opcode;
    replacement.dst = use_instr.dst;
    replacement.srcs = def_instr.srcs;
    return true;

  default:
    return false;
  }
}

std::vector<Instruction>
fold_temporaries(const std::vector<Instruction> &input) {
  std::unordered_map<std::string, int> def_indices;
  std::unordered_map<std::string, int> use_indices;
  std::unordered_map<std::string, int> use_counts;

  for (int i = 0; i < static_cast<int>(input.size()); i++) {
    const Instruction &instr = input[i];
    if (instr.dst.has_value() && instr.dst->type == OPERAND_TEMPORARY)
      def_indices[instr.dst->name] = i;
    for (const Operand &src: instr.srcs) {
      if (src.type != OPERAND_TEMPORARY)
        continue;
      use_counts[src.name]++;
      use_indices[src.name] = i;
    }
  }

  std::vector<Instruction> output = input;
  std::unordered_set<int> remove_indices;

  for (const auto &[temp_name, count]: use_counts) {
    if (count != 1)
      continue;
    auto def_it = def_indices.find(temp_name);
    auto use_it = use_indices.find(temp_name);
    if (def_it == def_indices.end() || use_it == use_indices.end())
      continue;

    const Instruction &def_instr = input[def_it->second];
    const Instruction &use_instr = input[use_it->second];

    Instruction replacement;
    if (!try_eliminate_single_use_temp(def_instr, use_instr, temp_name,
                                       replacement))
      continue;

    output[use_it->second] = replacement;
    remove_indices.insert(def_it->second);
  }

  std::vector<Instruction> cleaned;
  cleaned.reserve(output.size() - remove_indices.size());
  for (int i = 0; i < static_cast<int>(output.size()); i++) {
    if (remove_indices.count(i) == 0)
      cleaned.push_back(output[i]);
  }

  return cleaned;
}

static bool is_cse_opcode(Opcode op) {
  switch (op) {
  case OP_LOAD_OFFSET:
  case OP_ADD:
  case OP_SUB:
  case OP_MUL:
  case OP_DIV:
  case OP_CMP_EQ:
  case OP_CMP_NEQ:
  case OP_CMP_LT:
  case OP_CMP_LTE:
  case OP_CMP_GT:
  case OP_CMP_GTE:
    return true;
  default:
    return false;
  }
}

std::vector<Instruction> global_cse(const std::vector<Instruction> &input) {
  std::unordered_map<LoadKey, std::string, LoadKeyHash> load_cache{};
  std::unordered_map<ExprKey, std::string, ExprKeyHash> expr_cache{};
  std::unordered_map<std::string, std::string> addrof_cache{};
  std::vector<Instruction> output{};
  output.reserve(input.size());

  for (const Instruction &instr: input) {
    if (instr.opcode == OP_LABEL) {
      load_cache.clear();
      expr_cache.clear();
      addrof_cache.clear();
    }

    if (instr.opcode == OP_STORE_OFFSET && instr.srcs.size() == 3 &&
        instr.srcs[1].type == OPERAND_CONSTANT_INT) {
      LoadKey key = load_key(instr.srcs[0], instr.srcs[1].int_value);
      load_cache.erase(key);

      output.push_back(instr);
      continue;
    }

    if (instr.opcode == OP_CALL) {
      for (size_t a = 1; a < instr.srcs.size(); a++) {
        const Operand &arg = instr.srcs[a];
        if (arg.type == OPERAND_VARIABLE)
          erase_load_keys_for_base(load_cache, operand_key(arg));
      }
      output.push_back(instr);
      continue;
    }

    if (instr.opcode == OP_ADDROF && instr.dst.has_value() &&
        instr.dst->type == OPERAND_TEMPORARY && instr.srcs.size() == 1 &&
        instr.srcs[0].type == OPERAND_VARIABLE) {
      const std::string &key = instr.srcs[0].name;
      auto it = addrof_cache.find(key);
      if (it != addrof_cache.end()) {
        Instruction repl;
        repl.opcode = OP_ASSIGN;
        repl.dst = instr.dst;
        repl.srcs = {Operand::Temporary(it->second)};
        output.push_back(repl);
        continue;
      }
      addrof_cache[key] = instr.dst->name;
      output.push_back(instr);
      continue;
    }

    if (instr.opcode == OP_LOAD_OFFSET && instr.dst.has_value() &&
        instr.srcs.size() == 2 && instr.srcs[1].type == OPERAND_CONSTANT_INT) {
      LoadKey key = load_key(instr.srcs[0], instr.srcs[1].int_value);
      auto it = load_cache.find(key);
      if (it != load_cache.end()) {
        Instruction repl;
        repl.opcode = OP_ASSIGN;
        repl.dst = instr.dst;
        repl.srcs = {Operand::Temporary(it->second)};
        output.push_back(repl);
        continue;
      }

      if (instr.dst->type == OPERAND_TEMPORARY)
        load_cache[key] = instr.dst->name;
      output.push_back(instr);
      continue;
    }

    if (is_cse_opcode(instr.opcode) && instr.dst.has_value() &&
        instr.dst->type == OPERAND_TEMPORARY && instr.srcs.size() == 2 &&
        instr.opcode != OP_LOAD_OFFSET) {
      ExprKey key = expr_key(instr.opcode, instr.srcs[0], instr.srcs[1]);
      auto it = expr_cache.find(key);
      if (it != expr_cache.end()) {
        Instruction repl;
        repl.opcode = OP_ASSIGN;
        repl.dst = instr.dst;
        repl.srcs = {Operand::Temporary(it->second)};
        output.push_back(repl);
        continue;
      }

      expr_cache[key] = instr.dst->name;
      output.push_back(instr);
      continue;
    }

    output.push_back(instr);
  }

  return output;
}

std::vector<Instruction>
store_load_forward(const std::vector<Instruction> &input) {
  std::unordered_map<LoadKey, Operand, LoadKeyHash> stores{};
  std::vector<Instruction> output{};
  output.reserve(input.size());

  for (const Instruction &instr: input) {
    if (instr.opcode == OP_LABEL)
      stores.clear();

    if (instr.opcode == OP_STORE_OFFSET && instr.srcs.size() == 3 &&
        instr.srcs[1].type == OPERAND_CONSTANT_INT) {
      LoadKey key = load_key(instr.srcs[0], instr.srcs[1].int_value);
      stores[key] = instr.srcs[2];
      output.push_back(instr);
      continue;
    }

    if (instr.opcode == OP_LOAD_OFFSET && instr.dst.has_value() &&
        instr.srcs.size() == 2 && instr.srcs[1].type == OPERAND_CONSTANT_INT) {
      LoadKey key = load_key(instr.srcs[0], instr.srcs[1].int_value);
      auto it = stores.find(key);
      if (it != stores.end()) {
        Instruction repl;
        repl.opcode = OP_ASSIGN;
        repl.dst = instr.dst;
        repl.srcs = {it->second};
        output.push_back(repl);
        continue;
      }
    }

    output.push_back(instr);
  }

  return output;
}

struct FusedBranchCond {
  Operand cond;
  bool flip_branch = false;
};

static std::optional<FusedBranchCond>
fuse_cmp_branch_cond(const Instruction &cmp, bool jump_on_zero) {
  if (cmp.srcs.size() != 2 || cmp.srcs[1].type != OPERAND_CONSTANT_INT)
    return std::nullopt;

  const Operand &lhs = cmp.srcs[0];
  int64_t rhs = cmp.srcs[1].int_value;

  switch (cmp.opcode) {
  case OP_CMP_EQ:
    if (rhs == 0)
      return FusedBranchCond{lhs, jump_on_zero};
    break;
  case OP_CMP_NEQ:
    if (rhs == 0)
      return FusedBranchCond{lhs, !jump_on_zero};
    break;
  default:
    break;
  }

  return std::nullopt;
}

static void apply_fused_branch(Instruction &branch, const FusedBranchCond &fused) {
  branch.srcs[0] = fused.cond;
  if (fused.flip_branch) {
    branch.opcode = branch.opcode == OP_JMP_IF_ZERO ? OP_JMP_IF_NONZERO
                                                    : OP_JMP_IF_ZERO;
  }
}

static std::optional<Instruction>
fuse_var_cmp_branch(const Instruction &cmp, bool jump_on_zero,
                    const Operand &label) {
  if (cmp.srcs.size() != 2)
    return std::nullopt;

  Instruction fused;
  fused.dst = std::nullopt;
  fused.srcs = {cmp.srcs[0], cmp.srcs[1], label};

  switch (cmp.opcode) {
  case OP_CMP_EQ:
    fused.opcode = jump_on_zero ? OP_JMP_IF_NE : OP_JMP_IF_EQ;
    return fused;
  case OP_CMP_NEQ:
    fused.opcode = jump_on_zero ? OP_JMP_IF_EQ : OP_JMP_IF_NE;
    return fused;
  default:
    return std::nullopt;
  }
}

std::vector<Instruction>
fuse_cmp_branch(const std::vector<Instruction> &input) {
  std::vector<Instruction> output{};
  output.reserve(input.size());
  std::unordered_set<int> skip;

  for (int i = 0; i < static_cast<int>(input.size()); i++) {
    if (skip.count(i) != 0)
      continue;

    const Instruction &instr = input[i];

    if ((instr.opcode == OP_JMP_IF_ZERO || instr.opcode == OP_JMP_IF_NONZERO) &&
        instr.srcs.size() == 2 &&
        instr.srcs[0].type == OPERAND_TEMPORARY &&
        count_temp_uses(input, instr.srcs[0].name) == 1) {
      auto def_it = std::find_if(input.begin(), input.end(), [&](const Instruction &cand) {
        return cand.dst.has_value() && cand.dst->type == OPERAND_TEMPORARY &&
               cand.dst->name == instr.srcs[0].name &&
               (cand.opcode == OP_CMP_EQ || cand.opcode == OP_CMP_NEQ ||
                cand.opcode == OP_CMP_LT || cand.opcode == OP_CMP_LTE ||
                cand.opcode == OP_CMP_GT || cand.opcode == OP_CMP_GTE);
      });

      if (def_it != input.end()) {
        bool jump_on_zero = instr.opcode == OP_JMP_IF_ZERO;
        int def_idx = static_cast<int>(std::distance(input.begin(), def_it));
        if (auto fused = fuse_cmp_branch_cond(*def_it, jump_on_zero);
            fused.has_value()) {
          Instruction branch = instr;
          apply_fused_branch(branch, *fused);
          skip.insert(i);
          skip.insert(def_idx);
          output.push_back(branch);
          continue;
        }
        if (auto fused =
                fuse_var_cmp_branch(*def_it, jump_on_zero, instr.srcs[1]);
            fused.has_value()) {
          skip.insert(i);
          skip.insert(def_idx);
          output.push_back(*fused);
          continue;
        }
      }
    }

    if (instr.dst.has_value() && instr.dst->type == OPERAND_TEMPORARY &&
        count_temp_uses(input, instr.dst->name) == 1 && is_cmp_opcode(instr.opcode)) {
      for (int j = i + 1; j < static_cast<int>(input.size()); j++) {
        const Instruction &next = input[j];
        if (next.opcode == OP_LABEL)
          break;
        if ((next.opcode == OP_JMP_IF_ZERO || next.opcode == OP_JMP_IF_NONZERO) &&
            next.srcs.size() == 2 && next.srcs[0].type == OPERAND_TEMPORARY &&
            next.srcs[0].name == instr.dst->name) {
          bool jump_on_zero = next.opcode == OP_JMP_IF_ZERO;
          if (auto fused = fuse_cmp_branch_cond(instr, jump_on_zero);
              fused.has_value()) {
            Instruction branch = next;
            apply_fused_branch(branch, *fused);
            skip.insert(i);
            skip.insert(j);
            output.push_back(branch);
          } else if (auto var_fused =
                         fuse_var_cmp_branch(instr, jump_on_zero, next.srcs[1]);
                     var_fused.has_value()) {
            skip.insert(i);
            skip.insert(j);
            output.push_back(*var_fused);
          }
          break;
        }
        if (next.dst.has_value() && next.dst->type == OPERAND_TEMPORARY)
          break;
      }
      if (skip.count(i) != 0)
        continue;
    }

    output.push_back(instr);
  }

  return output;
}

static std::string resolve_label(
    std::string label,
    const std::unordered_map<std::string, std::string> &redirect) {
  while (redirect.contains(label))
    label = redirect.at(label);
  return label;
}

static bool is_branch_opcode(Opcode op) {
  return op == OP_JMP || op == OP_JMP_IF_ZERO || op == OP_JMP_IF_NONZERO ||
         op == OP_JMP_IF_EQ || op == OP_JMP_IF_NE;
}

static size_t branch_label_index(const Instruction &instr) {
  switch (instr.opcode) {
  case OP_JMP:
    return 0;
  case OP_JMP_IF_ZERO:
  case OP_JMP_IF_NONZERO:
    return 1;
  case OP_JMP_IF_EQ:
  case OP_JMP_IF_NE:
    return 2;
  default:
    std::unreachable();
  }
}

std::vector<Instruction>
simplify_cfg(const std::vector<Instruction> &input) {
  std::unordered_map<std::string, std::string> empty_label_redirect{};
  const int n = static_cast<int>(input.size());

  for (int i = 0; i < n; i++) {
    if (input[i].opcode != OP_LABEL)
      continue;

    bool empty = true;
    int j = i + 1;
    while (j < n) {
      if (input[j].opcode == OP_LABEL)
        break;
      if (input[j].opcode != OP_NOP) {
        empty = false;
        break;
      }
      j++;
    }

    if (empty && j < n && input[j].opcode == OP_LABEL) {
      const std::string &from = input[i].srcs[0].name;
      const std::string &to = input[j].srcs[0].name;
      if (!from.empty() && !to.empty())
        empty_label_redirect[from] = to;
    }
  }

  std::unordered_set<std::string> removed_labels;
  for (const auto &[from, _]: empty_label_redirect)
    removed_labels.insert(from);

  std::vector<Instruction> output{};
  output.reserve(input.size());

  for (const Instruction &instr: input) {
    if (instr.opcode == OP_LABEL) {
      if (removed_labels.count(instr.srcs[0].name) != 0)
        continue;
      Instruction label = instr;
      if (!output.empty() && output.back().opcode == OP_LABEL &&
          output.back().srcs[0].name == label.srcs[0].name)
        continue;
      output.push_back(label);
      continue;
    }

    if (!is_branch_opcode(instr.opcode))
      output.push_back(instr);
    else {
      Instruction branch = instr;
      size_t idx = branch_label_index(instr);
      Operand &target = branch.srcs[idx];
      std::string resolved =
          resolve_label(target.name, empty_label_redirect);
      if (!resolved.empty())
        target = Operand::Label(resolved);
      output.push_back(branch);
    }
  }

  return output;
}

std::vector<Instruction>
eliminate_copy_assigns(const std::vector<Instruction> &input) {
  std::unordered_map<std::string, int> use_counts;
  for (const Instruction &instr: input) {
    for (const Operand &src: instr.srcs) {
      if (src.type == OPERAND_TEMPORARY)
        use_counts[src.name]++;
    }
  }

  std::vector<Instruction> output{};
  output.reserve(input.size());

  for (const Instruction &instr: input) {
    if (instr.opcode == OP_ASSIGN && instr.dst.has_value() &&
        instr.dst->type == OPERAND_TEMPORARY &&
        use_counts[instr.dst->name] == 0)
      continue;

    output.push_back(instr);
  }

  return output;
}

std::vector<Instruction>
dead_code_elim(const std::vector<Instruction> &input) {
  std::unordered_map<std::string, int> use_counts;
  for (const Instruction &instr: input) {
    for (const Operand &src: instr.srcs) {
      if (src.type == OPERAND_TEMPORARY)
        use_counts[src.name]++;
    }
  }

  std::vector<Instruction> output{};
  output.reserve(input.size());
  bool skip_until_label = false;

  for (const Instruction &instr: input) {
    if (instr.opcode == OP_LABEL)
      skip_until_label = false;

    if (skip_until_label)
      continue;

    if (instr.opcode == OP_JMP)
      skip_until_label = true;

    if (instr.opcode == OP_NOP)
      continue;

    if (instr.dst.has_value() && instr.dst->type == OPERAND_TEMPORARY &&
        is_pure_opcode(instr.opcode) && use_counts[instr.dst->name] == 0)
      continue;

    output.push_back(instr);
  }

  return output;
}

static bool instructions_equal(const std::vector<Instruction> &a,
                               const std::vector<Instruction> &b) {
  if (a.size() != b.size())
    return false;
  for (size_t i = 0; i < a.size(); i++) {
    if (a[i].opcode != b[i].opcode)
      return false;
    if (a[i].dst.has_value() != b[i].dst.has_value())
      return false;
    if (a[i].dst.has_value() && !operands_equal(*a[i].dst, *b[i].dst))
      return false;
    if (a[i].srcs.size() != b[i].srcs.size())
      return false;
    for (size_t j = 0; j < a[i].srcs.size(); j++) {
      if (!operands_equal(a[i].srcs[j], b[i].srcs[j]))
        return false;
    }
  }
  return true;
}

void optimize_function(Function &fn) {
  constexpr int kMaxIterations = 12;

  for (int i = 0; i < kMaxIterations; i++) {
    std::vector<Instruction> before = fn.instructions;

    fn.instructions = fold_constants(fn.instructions);
    fn.instructions = copy_propagate(fn.instructions);
    fn.instructions = fold_constants(fn.instructions);
    fn.instructions = store_load_forward(fn.instructions);
    fn.instructions = global_cse(fn.instructions);
    fn.instructions = copy_propagate(fn.instructions);
    fn.instructions = fold_constants(fn.instructions);
    fn.instructions = fold_branches(fn.instructions);
    fn.instructions = fuse_cmp_branch(fn.instructions);
    fn.instructions = fold_temporaries(fn.instructions);
    fn.instructions = global_cse(fn.instructions);
    fn.instructions = copy_propagate(fn.instructions);
    fn.instructions = fold_constants(fn.instructions);
    fn.instructions = fold_branches(fn.instructions);
    fn.instructions = eliminate_copy_assigns(fn.instructions);
    fn.instructions = dead_code_elim(fn.instructions);
    fn.instructions = simplify_cfg(fn.instructions);

    if (instructions_equal(fn.instructions, before))
      break;
  }
}
