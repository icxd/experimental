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

std::vector<Instruction> fold_constants(const std::vector<Instruction> &input) {
  // std::vector<Instruction> output{};
  // for (const auto &instr: input) {
  //   switch (instr.opcode) {
  //   case OP_ADD: break;
  //   default:     break;
  //   }
  // }
  return input;
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
