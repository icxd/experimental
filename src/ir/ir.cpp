#include <functional>
#include <unordered_map>

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

std::vector<Instruction> fold_constants(const std::vector<Instruction> &input) {
  std::unordered_map<std::string, int64_t> temps{};
  std::vector<Instruction> output{};
  output.reserve(input.size());

  for (const Instruction &instr: input) {
    Instruction folded = instr;

    auto fold_binary = [&](auto fn) -> bool {
      if (!instr.dst.has_value() || instr.srcs.size() != 2)
        return false;
      auto lhs = known_operand_value(instr.srcs[0], temps);
      auto rhs = known_operand_value(instr.srcs[1], temps);
      if (!lhs.has_value() || !rhs.has_value())
        return false;
      folded.opcode = OP_ASSIGN;
      folded.srcs = {Operand::ConstantInt(fn(*lhs, *rhs))};
      return true;
    };

    switch (instr.opcode) {
    case OP_ADD:
      if (fold_binary(std::plus<>())) {
      } break;
    case OP_SUB:
      if (fold_binary(std::minus<>())) {
      } break;
    case OP_MUL:
      if (fold_binary(std::multiplies<>())) {
      } break;
    case OP_DIV:
      if (fold_binary(std::divides<>())) {
      } break;
    case OP_CMP_EQ:
      if (fold_binary([](int64_t a, int64_t b) { return a == b ? 1 : 0; })) {
      } break;
    case OP_CMP_NEQ:
      if (fold_binary([](int64_t a, int64_t b) { return a != b ? 1 : 0; })) {
      } break;
    case OP_CMP_LT:
      if (fold_binary([](int64_t a, int64_t b) { return a < b ? 1 : 0; })) {
      } break;
    case OP_CMP_LTE:
      if (fold_binary([](int64_t a, int64_t b) { return a <= b ? 1 : 0; })) {
      } break;
    case OP_CMP_GT:
      if (fold_binary([](int64_t a, int64_t b) { return a > b ? 1 : 0; })) {
      } break;
    case OP_CMP_GTE:
      if (fold_binary([](int64_t a, int64_t b) { return a >= b ? 1 : 0; })) {
      } break;
    default:
      break;
    }

    if (folded.opcode == OP_ASSIGN && folded.dst.has_value() &&
        folded.srcs.size() == 1 &&
        folded.srcs[0].type == OPERAND_CONSTANT_INT &&
        folded.dst->type == OPERAND_TEMPORARY)
      temps[folded.dst->name] = folded.srcs[0].int_value;

    output.push_back(folded);
  }

  return output;
}

std::vector<Instruction>
fold_temporaries(const std::vector<Instruction> &input) {
  auto live_ranges = compute_live_ranges(input);

  std::vector<Instruction> output = input;
  std::unordered_set<int> remove_indices;

  for (const auto &[temp_name, range]: live_ranges) {
    if (range.end == range.start + 1) {
      const Instruction &def_instr = input[range.start];
      const Instruction &use_instr = input[range.end];

      switch (def_instr.opcode) {
      case OP_ADDROF:
      case OP_DEREF:  {
        if (def_instr.dst && def_instr.srcs.size() == 1 && use_instr.dst &&
            use_instr.srcs.size() == 1 &&
            use_instr.srcs[0].type == OPERAND_TEMPORARY &&
            use_instr.srcs[0].name == temp_name) {
          Instruction new_instr;
          new_instr.opcode = def_instr.opcode;
          new_instr.dst = use_instr.dst;
          new_instr.srcs = def_instr.srcs;

          output[range.end] = new_instr;
          remove_indices.insert(range.start);
        }
      }

      case OP_ADD:
      case OP_SUB:
      case OP_MUL:
      case OP_DIV: {
        if (def_instr.dst && def_instr.srcs.size() == 2 && use_instr.dst &&
            use_instr.srcs.size() == 1 &&
            use_instr.srcs[0].type == OPERAND_TEMPORARY &&
            use_instr.srcs[0].name == temp_name) {
          Instruction new_instr;
          new_instr.opcode = def_instr.opcode;
          new_instr.dst = use_instr.dst;
          new_instr.srcs = def_instr.srcs;

          output[range.end] = new_instr;
          remove_indices.insert(range.start);
        }
      } break;

      case OP_CALL: {
        if (def_instr.dst && def_instr.srcs.size() > 0 && use_instr.dst &&
            use_instr.srcs.size() == 1 &&
            use_instr.srcs[0].type == OPERAND_TEMPORARY &&
            use_instr.srcs[0].name == temp_name) {
          Instruction new_instr;
          new_instr.opcode = def_instr.opcode;
          new_instr.dst = use_instr.dst;
          new_instr.srcs = def_instr.srcs;

          output[range.end] = new_instr;
          remove_indices.insert(range.start);
        }
      } break;

      default: break;
      }
    }
  }

  std::vector<Instruction> cleaned;
  for (int i = 0; i < output.size(); ++i) {
    if (remove_indices.count(i) == 0)
      cleaned.push_back(output[i]);
  }

  return cleaned;
}
