#include <utility>

#include <codegen/emitters/aarch64_macos_gas.hpp>
#include <codegen/regalloc.hpp>
#include <ir/ir.hpp>

void Aarch64MacosGasEmitter::emit() {
  _output += ".text\n";
  for (const auto *function: _functions)
    emit_function(*function);
}

static size_t count_local_variables(const Function &function) {
  std::set<std::string> local_vars;
  for (const auto &instr: function.instructions) {
    if (instr.dst.has_value() && instr.dst->type == OPERAND_VARIABLE) {
      local_vars.insert(instr.dst->name);
    }
    for (const auto &src: instr.srcs) {
      if (src.type == OPERAND_VARIABLE) {
        local_vars.insert(src.name);
      }
    }
  }
  return local_vars.size();
}

void Aarch64MacosGasEmitter::emit_function(Function function) {
  std::string name(function.name);
  _current_fn = name;
  _stack_loc.clear();
  _spill_loc.clear();
  _next_stack_loc = 16;

  if (function.linkage == LINK_INTERN) {
    auto regmap = allocate_registers(function.instructions, _registers);
    _register_maps.insert({name, regmap});

    size_t spill_count = 0;
    for (const auto &[_, alloc]: regmap) {
      if (alloc.spilled)
        spill_count++;
    }

    size_t local_vars = count_local_variables(function);
    size_t stack_slots = local_vars + function.parameters.size() + spill_count;
    size_t stack_size = stack_slots * 16;
    if (stack_size < 16)
      stack_size = 16;

    _output += ".globl " + name + "\n";
    _output += name + ":\n";

    // If the function contains a call instruction, we need to ensure that
    // the stack pointer is aligned to 16 bytes.

    bool has_call_instruction = false;
    for (const auto &instr: function.instructions) {
      if (instr.opcode == OP_CALL) {
        has_call_instruction = true;
        break;
      }
    }

    if (has_call_instruction) {
      _output += "  stp x29, x30, [sp, -" + std::to_string(stack_size) + "]!\n";
      _output += "  mov x29, sp\n";
    } else if (local_vars > 0) {
      _output += "  sub sp, sp, " + std::to_string(stack_size) + "\n";
    }

    for (size_t i = 0; i < function.parameters.size(); i++) {
      std::string_view param = function.parameters[i];
      _stack_loc.insert({std::string(param), _next_stack_loc});
      _output += "  str x" + std::to_string(i) + ", [sp, #" +
                 std::to_string(_next_stack_loc) + "]\n";
      _next_stack_loc += 16;
    }

    for (const auto &[temp, alloc]: regmap) {
      if (alloc.spilled) {
        _spill_loc[temp] = _next_stack_loc;
        _next_stack_loc += 8;
      }
    }

    _end_label = ".L_end_" + name;
    for (const auto &instr: function.instructions) {
      emit_instruction(instr);
    }

    _output += _end_label + ":\n";

    if (has_call_instruction)
      _output += "  ldp x29, x30, [sp], " + std::to_string(stack_size) + "\n";
    else if (local_vars > 0)
      _output += "  add sp, sp, " + std::to_string(stack_size) + "\n";

    _output += "  ret\n";
  } else {
    _output += ".extern " + name + "\n";
  }
}

void Aarch64MacosGasEmitter::emit_instruction(Instruction instr) {
  _output += "  // " + instr.to_string() + "\n";

  if (instr.dst.has_value() && instr.dst->type == OPERAND_VARIABLE) {
    _stack_loc.insert({instr.dst->name, _next_stack_loc});
    _next_stack_loc += 8;
  }

  switch (instr.opcode) {
  case OP_NOP:    break;

  case OP_ASSIGN: {
    assert(instr.dst.has_value());
    Operand dst = *instr.dst;

    assert(instr.srcs.size() == 1);
    Operand src = instr.srcs[0];

    _output += "  mov x9, " + emit_operand(src) + "\n";
    _output += "  str x9, [sp, " + emit_operand(dst) + "]\n";
  } break;

  case OP_ADDROF: {
    assert(instr.dst.has_value());
    Operand dst = *instr.dst;

    assert(instr.srcs.size() == 1);
    Operand src = instr.srcs[0];

    _output += "  add x9, sp, " + emit_operand(src) + "\n";

    if (dst.type == OPERAND_VARIABLE)
      _output += "  str x9, [sp, " + emit_operand(dst) + "]\n";
    else
      _output += "  mov " + emit_operand(dst) + ", x9\n";
  } break;

  case OP_DEREF: {
    assert(instr.dst.has_value());
    Operand dst = *instr.dst;

    assert(instr.srcs.size() == 1);
    Operand src = instr.srcs[0];

    _output += "  ldr x9, [sp, " + emit_operand(src) + "]\n";
    _output += "  ldr x9, [x9]\n";

    if (dst.type == OPERAND_VARIABLE)
      _output += "  str x9, [sp, " + emit_operand(dst) + "]\n";
    else
      _output += "  mov " + emit_operand(dst) + ", x9\n";
  } break;

  case OP_ADD: {
    assert(instr.dst.has_value());
    Operand dst = *instr.dst;

    assert(instr.srcs.size() == 2);
    Operand src1 = instr.srcs[0];
    Operand src2 = instr.srcs[1];

    _output += "  mov x10, " + emit_operand(src1) + "\n";
    _output += "  mov x11, " + emit_operand(src2) + "\n";
    _output += "  add x9, x10, x11\n";

    if (dst.type == OPERAND_VARIABLE)
      _output += "  str x9, [sp, " + emit_operand(dst) + "]\n";
    else
      _output += "  mov " + emit_operand(dst) + ", x9\n";
  } break;

  case OP_SUB: {
    assert(instr.dst.has_value());
    Operand dst = *instr.dst;
    Operand src1 = instr.srcs[0];
    Operand src2 = instr.srcs[1];
    _output += "  mov x10, " + emit_operand(src1) + "\n";
    _output += "  mov x11, " + emit_operand(src2) + "\n";
    _output += "  sub x9, x10, x11\n";
    if (dst.type == OPERAND_VARIABLE)
      _output += "  str x9, [sp, " + emit_operand(dst) + "]\n";
    else
      _output += "  mov " + emit_operand(dst) + ", x9\n";
  } break;

  case OP_MUL: {
    assert(instr.dst.has_value());
    Operand dst = *instr.dst;
    Operand src1 = instr.srcs[0];
    Operand src2 = instr.srcs[1];
    _output += "  mov x10, " + emit_operand(src1) + "\n";
    _output += "  mov x11, " + emit_operand(src2) + "\n";
    _output += "  mul x9, x10, x11\n";
    if (dst.type == OPERAND_VARIABLE)
      _output += "  str x9, [sp, " + emit_operand(dst) + "]\n";
    else
      _output += "  mov " + emit_operand(dst) + ", x9\n";
  } break;

  case OP_DIV: {
    assert(instr.dst.has_value());
    Operand dst = *instr.dst;
    Operand src1 = instr.srcs[0];
    Operand src2 = instr.srcs[1];
    _output += "  mov x10, " + emit_operand(src1) + "\n";
    _output += "  mov x11, " + emit_operand(src2) + "\n";
    _output += "  sdiv x9, x10, x11\n";
    if (dst.type == OPERAND_VARIABLE)
      _output += "  str x9, [sp, " + emit_operand(dst) + "]\n";
    else
      _output += "  mov " + emit_operand(dst) + ", x9\n";
  } break;

  case OP_CMP_EQ:
  case OP_CMP_NEQ:
  case OP_CMP_LT:
  case OP_CMP_LTE:
  case OP_CMP_GT:
  case OP_CMP_GTE: {
    assert(instr.dst.has_value());
    Operand dst = *instr.dst;
    Operand src1 = instr.srcs[0];
    Operand src2 = instr.srcs[1];
    const char *cond = nullptr;
    switch (instr.opcode) {
    case OP_CMP_EQ:  cond = "eq"; break;
    case OP_CMP_NEQ: cond = "ne"; break;
    case OP_CMP_LT:  cond = "lt"; break;
    case OP_CMP_LTE: cond = "le"; break;
    case OP_CMP_GT:  cond = "gt"; break;
    case OP_CMP_GTE: cond = "ge"; break;
    default: break;
    }
    _output += "  mov x10, " + emit_operand(src1) + "\n";
    _output += "  mov x11, " + emit_operand(src2) + "\n";
    _output += "  cmp x10, x11\n";
    _output += std::format("  cset x9, {}\n", cond);
    if (dst.type == OPERAND_VARIABLE)
      _output += "  str x9, [sp, " + emit_operand(dst) + "]\n";
    else
      _output += "  mov " + emit_operand(dst) + ", x9\n";
  } break;

  case OP_LABEL: {
    _output += emit_operand(instr.srcs[0]) + ":\n";
  } break;

  case OP_JMP: {
    _output += "  b " + emit_operand(instr.srcs[0]) + "\n";
  } break;

  case OP_JMP_IF_ZERO: {
    Operand cond = instr.srcs[0];
    if (cond.type == OPERAND_VARIABLE)
      _output += "  ldr x9, [sp, " + emit_operand(cond) + "]\n";
    else
      _output += "  mov x9, " + emit_operand(cond) + "\n";
    _output += "  cbz x9, " + emit_operand(instr.srcs[1]) + "\n";
  } break;

  case OP_CALL: {
    assert(instr.dst.has_value());
    Operand dst = *instr.dst;

    assert(instr.srcs.size() >= 1);
    Operand name = instr.srcs[0];

    for (size_t i = 1; i < instr.srcs.size(); i++) {
      Operand arg = instr.srcs[i];

      if (dst.type == OPERAND_VARIABLE)
        _output += "  ldr x" + std::to_string(i - 1) + ", [sp, " +
                   emit_operand(arg) + "]\n";
      else
        _output +=
            "  mov " + emit_operand(arg) + ", x" + std::to_string(i - 1) + "\n";
    }
    _output += "  bl " + emit_operand(name) + "\n";

    if (dst.type == OPERAND_VARIABLE)
      _output += "  str x0, [sp, " + emit_operand(dst) + "]\n";
    else
      _output += "  mov " + emit_operand(dst) + ", x0\n";
  } break;

  case OP_RET: {
    if (!instr.srcs.empty()) {
      Operand src = instr.srcs[0];
      if (src.type == OPERAND_VARIABLE)
        _output += "  ldr x0, [sp, " + emit_operand(src) + "]\n";
      else
        _output += "  mov x0, " + emit_operand(src) + "\n";
    }
    _output += "  b " + _end_label + "\n";
  } break;
  }
}

std::string Aarch64MacosGasEmitter::emit_value(Operand operand) {
  switch (operand.type) {
  case OPERAND_CONSTANT_INT: return std::format("#{}", operand.int_value);
  case OPERAND_CONSTANT:     return emit_value(*get_constant(operand.name));
  case OPERAND_VARIABLE:
    return "[sp, " + std::to_string(_stack_loc.at(operand.name)) + "]";
  case OPERAND_TEMPORARY: {
    const auto &alloc = current_alloc().at(operand.name);
    if (alloc.spilled)
      return "[sp, " + std::to_string(_spill_loc.at(operand.name)) + "]";
    return alloc.reg;
  }
  default: PANIC("unexpected operand in value position");
  }
}

void Aarch64MacosGasEmitter::store_scratch(const Operand &dst,
                                           const std::string &scratch) {
  if (dst.type == OPERAND_VARIABLE)
    _output += "  str " + scratch + ", [sp, " + emit_operand(dst) + "]\n";
  else if (dst.type == OPERAND_TEMPORARY) {
    const auto &alloc = current_alloc().at(dst.name);
    if (alloc.spilled)
      _output += "  str " + scratch + ", [sp, " +
                 std::to_string(_spill_loc.at(dst.name)) + "]\n";
    else
      _output += "  mov " + alloc.reg + ", " + scratch + "\n";
  }
}

std::string Aarch64MacosGasEmitter::emit_operand(Operand operand) {
  switch (operand.type) {
  case OPERAND_CONSTANT_INT: return std::format("{}", operand.int_value);
  case OPERAND_CONSTANT:     return emit_operand(*get_constant(operand.name));
  case OPERAND_VARIABLE:     return std::format("{}", _stack_loc.at(operand.name));
  case OPERAND_TEMPORARY: {
    const auto &alloc = current_alloc().at(operand.name);
    if (alloc.spilled)
      return std::to_string(_spill_loc.at(operand.name));
    return alloc.reg;
  }
  case OPERAND_FUNCTION: return operand.name;
  case OPERAND_LABEL:    return operand.name;
  }
  std::unreachable();
}
