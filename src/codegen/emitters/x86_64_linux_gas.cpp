#include <utility>

#include <codegen/emitters/x86_64_linux_gas.hpp>
#include <codegen/regalloc.hpp>
#include <ir/ir.hpp>

static const char *param_reg(size_t i) {
  static const char *regs[] = {"rdi", "rsi", "rdx", "rcx", "r8", "r9"};
  return i < 6 ? regs[i] : "rdi";
}

static size_t count_local_variables(const Function &function) {
  std::set<std::string> local_vars;
  for (const auto &instr: function.instructions) {
    if (instr.dst.has_value() && instr.dst->type == OPERAND_VARIABLE)
      local_vars.insert(instr.dst->name);
    for (const auto &src: instr.srcs) {
      if (src.type == OPERAND_VARIABLE)
        local_vars.insert(src.name);
    }
  }
  return local_vars.size();
}

void X86_64LinuxGasEmitter::emit() {
  _output += ".intel_syntax noprefix\n";
  _output += ".text\n";
  for (const auto *function: _functions)
    emit_function(*function);
  _output += ".section .note.GNU-stack,\"\",@progbits\n";
}

void X86_64LinuxGasEmitter::emit_function(Function function) {
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

    bool has_call_instruction = false;
    for (const auto &instr: function.instructions) {
      if (instr.opcode == OP_CALL) {
        has_call_instruction = true;
        break;
      }
    }

    if (has_call_instruction || local_vars > 0) {
      _output += "  push rbp\n";
      _output += "  mov rbp, rsp\n";
      if (stack_size > 0)
        _output += "  sub rsp, " + std::to_string(stack_size) + "\n";
    }

    for (size_t i = 0; i < function.parameters.size(); i++) {
      std::string_view param = function.parameters[i];
      _stack_loc.insert({std::string(param), _next_stack_loc});
      _output += "  mov [rsp + " + std::to_string(_next_stack_loc) + "], " +
                 param_reg(i) + "\n";
      _next_stack_loc += 16;
    }

    for (const auto &[temp, alloc]: regmap) {
      if (alloc.spilled) {
        _spill_loc[temp] = _next_stack_loc;
        _next_stack_loc += 8;
      }
    }

    _end_label = ".L_end_" + name;
    for (const auto &instr: function.instructions)
      emit_instruction(instr);

    _output += _end_label + ":\n";

    if (has_call_instruction || local_vars > 0) {
      _output += "  mov rsp, rbp\n";
      _output += "  pop rbp\n";
    }

    _output += "  ret\n";
  } else {
    _output += ".extern " + name + "\n";
  }
}

void X86_64LinuxGasEmitter::emit_instruction(Instruction instr) {
  _output += "  // " + instr.to_string() + "\n";

  if (instr.dst.has_value() && instr.dst->type == OPERAND_VARIABLE) {
    _stack_loc.insert({instr.dst->name, _next_stack_loc});
    _next_stack_loc += 8;
  }

  switch (instr.opcode) {
  case OP_NOP: break;

  case OP_ASSIGN: {
    assert(instr.dst.has_value());
    Operand dst = *instr.dst;
    assert(instr.srcs.size() == 1);
    Operand src = instr.srcs[0];

    _output += "  mov r10, " + emit_value(src) + "\n";
    store_scratch(dst, "r10");
  } break;

  case OP_ADDROF: {
    assert(instr.dst.has_value());
    Operand dst = *instr.dst;
    assert(instr.srcs.size() == 1);
    Operand src = instr.srcs[0];

    _output += "  lea r10, [rsp + " + emit_operand(src) + "]\n";

    store_scratch(dst, "r10");
  } break;

  case OP_DEREF: {
    assert(instr.dst.has_value());
    Operand dst = *instr.dst;
    assert(instr.srcs.size() == 1);
    Operand src = instr.srcs[0];

    _output += "  mov r10, " + emit_value(src) + "\n";
    _output += "  mov r10, [r10]\n";

    store_scratch(dst, "r10");
  } break;

  case OP_ADD: {
    assert(instr.dst.has_value());
    Operand dst = *instr.dst;
    assert(instr.srcs.size() == 2);
    Operand src1 = instr.srcs[0];
    Operand src2 = instr.srcs[1];

    _output += "  mov r10, " + emit_value(src1) + "\n";
    _output += "  mov r11, " + emit_value(src2) + "\n";
    _output += "  add r10, r11\n";
    store_scratch(dst, "r10");
  } break;

  case OP_SUB: {
    assert(instr.dst.has_value());
    Operand dst = *instr.dst;
    Operand src1 = instr.srcs[0];
    Operand src2 = instr.srcs[1];
    _output += "  mov r10, " + emit_value(src1) + "\n";
    _output += "  mov r11, " + emit_value(src2) + "\n";
    _output += "  sub r10, r11\n";
    store_scratch(dst, "r10");
  } break;

  case OP_MUL: {
    assert(instr.dst.has_value());
    Operand dst = *instr.dst;
    Operand src1 = instr.srcs[0];
    Operand src2 = instr.srcs[1];
    _output += "  mov r10, " + emit_value(src1) + "\n";
    _output += "  mov r11, " + emit_value(src2) + "\n";
    _output += "  imul r10, r11\n";
    store_scratch(dst, "r10");
  } break;

  case OP_DIV: {
    assert(instr.dst.has_value());
    Operand dst = *instr.dst;
    Operand src1 = instr.srcs[0];
    Operand src2 = instr.srcs[1];
    _output += "  mov rax, " + emit_value(src1) + "\n";
    _output += "  cqo\n";
    _output += "  mov rcx, " + emit_value(src2) + "\n";
    _output += "  idiv rcx\n";
    store_scratch(dst, "rax");
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
    const char *set = nullptr;
    switch (instr.opcode) {
    case OP_CMP_EQ:  set = "sete"; break;
    case OP_CMP_NEQ: set = "setne"; break;
    case OP_CMP_LT:  set = "setl"; break;
    case OP_CMP_LTE: set = "setle"; break;
    case OP_CMP_GT:  set = "setg"; break;
    case OP_CMP_GTE: set = "setge"; break;
    default: break;
    }
    _output += "  mov r10, " + emit_value(src1) + "\n";
    _output += "  mov r11, " + emit_value(src2) + "\n";
    _output += "  cmp r10, r11\n";
    _output += std::format("  {} r10b\n", set);
    _output += "  movzx r10, r10b\n";
    store_scratch(dst, "r10");
  } break;

  case OP_LABEL: {
    _output += emit_operand(instr.srcs[0]) + ":\n";
  } break;

  case OP_JMP: {
    _output += "  jmp " + emit_operand(instr.srcs[0]) + "\n";
  } break;

  case OP_JMP_IF_ZERO: {
    Operand cond = instr.srcs[0];
    if (cond.type == OPERAND_VARIABLE)
      _output += "  cmp qword ptr " + emit_value(cond) + ", 0\n";
    else if (cond.type == OPERAND_TEMPORARY)
      _output += "  test " + emit_value(cond) + ", " + emit_value(cond) + "\n";
    else {
      _output += "  mov r10, " + emit_value(cond) + "\n";
      _output += "  test r10, r10\n";
    }
    _output += "  je " + emit_operand(instr.srcs[1]) + "\n";
  } break;

  case OP_CALL: {
    assert(instr.dst.has_value());
    Operand dst = *instr.dst;
    assert(instr.srcs.size() >= 1);
    Operand name = instr.srcs[0];

    for (size_t i = 1; i < instr.srcs.size(); i++) {
      Operand arg = instr.srcs[i];
      std::string reg = param_reg(i - 1);
      if (arg.type == OPERAND_VARIABLE)
        _output += "  mov " + reg + ", [rsp + " + emit_operand(arg) + "]\n";
      else
        _output += "  mov " + reg + ", " + emit_value(arg) + "\n";
    }

    _output += "  call " + emit_operand(name) + "\n";

    store_scratch(dst, "rax");
  } break;

  case OP_RET: {
    if (!instr.srcs.empty()) {
      Operand src = instr.srcs[0];
      if (src.type == OPERAND_VARIABLE)
        _output += "  mov rax, [rsp + " + emit_operand(src) + "]\n";
      else
        _output += "  mov rax, " + emit_value(src) + "\n";
    }
    _output += "  jmp " + _end_label + "\n";
  } break;
  }
}

std::string X86_64LinuxGasEmitter::emit_value(Operand operand) {
  switch (operand.type) {
  case OPERAND_CONSTANT_INT: return std::format("{}", operand.int_value);
  case OPERAND_CONSTANT:     return emit_value(*get_constant(operand.name));
  case OPERAND_VARIABLE:
    return "[rsp + " + std::to_string(_stack_loc.at(operand.name)) + "]";
  case OPERAND_TEMPORARY: {
    const auto &alloc = current_alloc().at(operand.name);
    if (alloc.spilled)
      return "[rsp + " + std::to_string(_spill_loc.at(operand.name)) + "]";
    return alloc.reg;
  }
  default: PANIC("unexpected operand in value position");
  }
}

void X86_64LinuxGasEmitter::store_scratch(const Operand &dst,
                                          const std::string &scratch) {
  if (dst.type == OPERAND_VARIABLE) {
    _output += "  mov [rsp + " + emit_operand(dst) + "], " + scratch + "\n";
  } else if (dst.type == OPERAND_TEMPORARY) {
    const auto &alloc = current_alloc().at(dst.name);
    if (alloc.spilled)
      _output += "  mov [rsp + " + std::to_string(_spill_loc.at(dst.name)) +
                 "], " + scratch + "\n";
    else
      _output += "  mov " + alloc.reg + ", " + scratch + "\n";
  }
}

std::string X86_64LinuxGasEmitter::emit_operand(Operand operand) {
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
