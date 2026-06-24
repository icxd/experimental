#include <utility>

#include <codegen/emitters/x86_64_linux_gas.hpp>
#include <codegen/regalloc.hpp>
#include <ir/ir.hpp>

static const char *param_reg(size_t i) {
  static const char *regs[] = {"rdi", "rsi", "rdx", "rcx", "r8", "r9"};
  return i < 6 ? regs[i] : "rdi";
}

static bool var_is_indirect(const Function *fn, std::string_view name) {
  return fn != nullptr && fn->indirect_vars.contains(std::string(name));
}

static bool var_is_struct_value(const Function *fn, std::string_view name) {
  return fn != nullptr && fn->struct_value_params.contains(std::string(name));
}

static void emit_struct_param_copy(std::string &output, const std::string &src_reg,
                                   size_t dest_off, size_t size) {
  output += "  mov rax, " + src_reg + "\n";
  for (size_t off = 0; off < size; off += 8) {
    output += "  mov rcx, [rax + " + std::to_string(off) + "]\n";
    output += "  mov [rsp + " + std::to_string(dest_off + off) + "], rcx\n";
  }
}

static size_t variable_stack_size(const Function &function,
                                  std::string_view name) {
  auto it = function.variable_sizes.find(std::string(name));
  if (it != function.variable_sizes.end())
    return it->second;
  return 8;
}

static size_t compute_locals_size(const Function &function) {
  std::set<std::string> local_vars;
  for (const auto &instr: function.instructions) {
    if (instr.dst.has_value() && instr.dst->type == OPERAND_VARIABLE)
      local_vars.insert(instr.dst->name);
    for (const auto &src: instr.srcs) {
      if (src.type == OPERAND_VARIABLE || src.type == OPERAND_STACK_ADDR)
        local_vars.insert(src.name);
    }
  }

  size_t total = 0;
  for (const auto &name: local_vars)
    total += variable_stack_size(function, name);
  return total;
}

void X86_64LinuxGasEmitter::emit() {
  _output += ".intel_syntax noprefix\n";
  _output += ".text\n";
  for (const auto *function: _functions)
    emit_function(*function);
  if (!_rodata.empty()) {
    _output += ".section .rodata\n";
    for (const auto &entry: _rodata) {
      _output += entry.label + ":\n";
      _output += "  .asciz \"";
      for (unsigned char ch: entry.bytes) {
        if (ch == '\n')
          _output += "\\n";
        else if (ch == '\t')
          _output += "\\t";
        else if (ch == '\r')
          _output += "\\r";
        else if (ch == '\\')
          _output += "\\\\";
        else if (ch == '"')
          _output += "\\\"";
        else if (ch == '\0')
          break;
        else if (std::isprint(ch))
          _output += static_cast<char>(ch);
        else
          _output += std::format("\\{:03o}", ch);
      }
      _output += "\"\n";
    }
  }
  _output += ".section .note.GNU-stack,\"\",@progbits\n";
}

void X86_64LinuxGasEmitter::emit_function(Function function) {
  std::string name(function.name);
  _current_fn = name;
  _current_function = &function;
  _stack_loc.clear();
  _spill_loc.clear();
  _var_sizes.clear();
  _next_stack_loc = 16;

  if (function.linkage == LINK_INTERN) {
    auto regmap = allocate_registers(function.instructions, _registers);
    _register_maps.insert({name, regmap});

    size_t spill_count = 0;
    for (const auto &[_, alloc]: regmap) {
      if (alloc.spilled)
        spill_count++;
    }

    size_t local_vars = compute_locals_size(function) > 0 ? 1 : 0;

    _output += ".globl " + name + "\n";
    _output += name + ":\n";

    bool has_call_instruction = false;
    for (const auto &instr: function.instructions) {
      if (instr.opcode == OP_CALL) {
        has_call_instruction = true;
        break;
      }
    }

    for (size_t i = 0; i < function.parameters.size(); i++) {
      std::string param(function.parameters[i]);
      _stack_loc.insert({param, _next_stack_loc});
      _next_stack_loc += variable_stack_size(function, param);
    }

    for (const auto &[temp, alloc]: regmap) {
      if (alloc.spilled) {
        _spill_loc[temp] = _next_stack_loc;
        _next_stack_loc += 8;
      }
    }

    for (const auto &instr: function.instructions) {
      auto layout_var = [&](const std::string &var_name) {
        if (_stack_loc.contains(var_name))
          return;
        size_t size = variable_stack_size(function, var_name);
        _stack_loc.insert({var_name, _next_stack_loc});
        _var_sizes.insert({var_name, size});
        _next_stack_loc += size;
      };
      if (instr.dst.has_value() && instr.dst->type == OPERAND_VARIABLE)
        layout_var(instr.dst->name);
      for (const auto &src: instr.srcs) {
        if (src.type == OPERAND_VARIABLE || src.type == OPERAND_STACK_ADDR)
          layout_var(src.name);
      }
    }

    size_t stack_size = _next_stack_loc;
    if (stack_size < 16)
      stack_size = 16;

    if (has_call_instruction || local_vars > 0) {
      _output += "  push rbp\n";
      _output += "  mov rbp, rsp\n";
      if (stack_size > 0)
        _output += "  sub rsp, " + std::to_string(stack_size) + "\n";
    }

    _next_stack_loc = 16;
    _stack_loc.clear();
    _var_sizes.clear();

    for (size_t i = 0; i < function.parameters.size(); i++) {
      std::string param(function.parameters[i]);
      size_t stack_off = _next_stack_loc;
      _stack_loc.insert({param, stack_off});
      if (var_is_struct_value(&function, param) ||
          variable_stack_size(function, param) > 8) {
        emit_struct_param_copy(_output, param_reg(i), stack_off,
                               variable_stack_size(function, param));
      } else {
        _output += "  mov [rsp + " + std::to_string(stack_off) + "], " +
                   param_reg(i) + "\n";
      }
      _next_stack_loc += variable_stack_size(function, param);
    }

    for (const auto &[temp, alloc]: regmap) {
      if (alloc.spilled) {
        _spill_loc[temp] = _next_stack_loc;
        _next_stack_loc += 8;
      }
    }

    for (const auto &instr: function.instructions) {
      auto layout_var = [&](const std::string &var_name) {
        if (_stack_loc.contains(var_name))
          return;
        size_t size = variable_stack_size(function, var_name);
        _stack_loc.insert({var_name, _next_stack_loc});
        _var_sizes.insert({var_name, size});
        _next_stack_loc += size;
      };
      if (instr.dst.has_value() && instr.dst->type == OPERAND_VARIABLE)
        layout_var(instr.dst->name);
      for (const auto &src: instr.srcs) {
        if (src.type == OPERAND_VARIABLE || src.type == OPERAND_STACK_ADDR)
          layout_var(src.name);
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

  auto ensure_var = [&](const std::string &var_name) {
    if (_stack_loc.contains(var_name))
      return;
    size_t size =
        _current_function != nullptr
            ? variable_stack_size(*_current_function, var_name)
            : 8;
    _stack_loc.insert({var_name, _next_stack_loc});
    _var_sizes.insert({var_name, size});
    _next_stack_loc += size;
  };

  if (instr.dst.has_value() && instr.dst->type == OPERAND_VARIABLE)
    ensure_var(instr.dst->name);
  for (const auto &src: instr.srcs) {
    if (src.type == OPERAND_VARIABLE || src.type == OPERAND_STACK_ADDR)
      ensure_var(src.name);
  }

  auto load_into = [&](const std::string &reg, const Operand &src) {
    if (src.type == OPERAND_STACK_ADDR) {
      _output += "  lea " + reg + ", [rsp + " + emit_operand(src) + "]\n";
      return;
    }
    if (src.type == OPERAND_LABEL || src.type == OPERAND_FUNCTION) {
      _output += "  lea " + reg + ", [rip + " + emit_operand(src) + "]\n";
      return;
    }
    if (src.type == OPERAND_VARIABLE ||
        (src.type == OPERAND_TEMPORARY &&
         current_alloc().at(src.name).spilled))
      _output += "  mov " + reg + ", qword ptr " + emit_value(src) + "\n";
    else
      _output += "  mov " + reg + ", " + emit_value(src) + "\n";
  };

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

  case OP_LOAD_OFFSET: {
    assert(instr.dst.has_value());
    Operand dst = *instr.dst;
    Operand base = instr.srcs[0];
    int64_t offset = instr.srcs[1].int_value;
    if (base.type == OPERAND_VARIABLE &&
        !var_is_indirect(_current_function, base.name)) {
      size_t base_off = _stack_loc.at(base.name);
      _output += "  mov r10, [rsp + " + std::to_string(base_off + offset) +
                 "]\n";
    } else {
      if (base.type == OPERAND_VARIABLE) {
        size_t base_off = _stack_loc.at(base.name);
        _output += "  mov r10, [rsp + " + std::to_string(base_off) + "]\n";
      } else {
        _output += "  mov r10, " + emit_value(base) + "\n";
      }
      if (offset != 0)
        _output += "  add r10, " + std::to_string(offset) + "\n";
      _output += "  mov r10, [r10]\n";
    }
    store_scratch(dst, "r10");
  } break;

  case OP_STORE_OFFSET: {
    Operand base = instr.srcs[0];
    int64_t offset = instr.srcs[1].int_value;
    Operand value = instr.srcs[2];
    _output += "  mov r11, " + emit_value(value) + "\n";
    if (base.type == OPERAND_VARIABLE &&
        !var_is_indirect(_current_function, base.name)) {
      size_t base_off = _stack_loc.at(base.name);
      _output += "  mov [rsp + " + std::to_string(base_off + offset) +
                 "], r11\n";
    } else {
      if (base.type == OPERAND_VARIABLE) {
        size_t base_off = _stack_loc.at(base.name);
        _output += "  mov r10, [rsp + " + std::to_string(base_off) + "]\n";
      } else {
        _output += "  mov r10, " + emit_value(base) + "\n";
      }
      if (offset != 0)
        _output += "  add r10, " + std::to_string(offset) + "\n";
      _output += "  mov [r10], r11\n";
    }
  } break;

  case OP_LOAD_BYTE: {
    assert(instr.dst.has_value());
    Operand dst = *instr.dst;
    assert(instr.srcs.size() == 1);
    _output += "  mov r10, " + emit_value(instr.srcs[0]) + "\n";
    _output += "  movzx r10, BYTE PTR [r10]\n";
    store_scratch(dst, "r10");
  } break;

  case OP_STORE_BYTE: {
    assert(instr.srcs.size() == 2);
    _output += "  mov r11, " + emit_value(instr.srcs[1]) + "\n";
    _output += "  mov r10, " + emit_value(instr.srcs[0]) + "\n";
    _output += "  mov BYTE PTR [r10], r11b\n";
  } break;

  case OP_LOAD_LABEL: {
    assert(instr.dst.has_value());
    Operand dst = *instr.dst;
    _output += "  lea r10, [rip + " + emit_operand(instr.srcs[0]) + "]\n";
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
    load_into("r10", instr.srcs[0]);
    _output += "  test r10, r10\n";
    _output += "  je " + emit_operand(instr.srcs[1]) + "\n";
  } break;

  case OP_JMP_IF_NONZERO: {
    load_into("r10", instr.srcs[0]);
    _output += "  test r10, r10\n";
    _output += "  jne " + emit_operand(instr.srcs[1]) + "\n";
  } break;

  case OP_JMP_IF_EQ: {
    _output += "  mov r10, " + emit_value(instr.srcs[0]) + "\n";
    _output += "  cmp r10, " + emit_value(instr.srcs[1]) + "\n";
    _output += "  je " + emit_operand(instr.srcs[2]) + "\n";
  } break;

  case OP_JMP_IF_NE: {
    _output += "  mov r10, " + emit_value(instr.srcs[0]) + "\n";
    _output += "  cmp r10, " + emit_value(instr.srcs[1]) + "\n";
    _output += "  jne " + emit_operand(instr.srcs[2]) + "\n";
  } break;

  case OP_CALL: {
    assert(instr.dst.has_value());
    Operand dst = *instr.dst;
    assert(instr.srcs.size() >= 1);
    Operand name = instr.srcs[0];

    for (size_t i = 1; i < instr.srcs.size(); i++) {
      Operand arg = instr.srcs[i];
      std::string reg = param_reg(i - 1);
      if (arg.type == OPERAND_STACK_ADDR)
        _output += "  lea " + reg + ", [rsp + " + emit_operand(arg) + "]\n";
      else if (arg.type == OPERAND_VARIABLE)
        _output += "  mov " + reg + ", [rsp + " + emit_operand(arg) + "]\n";
      else if (arg.type == OPERAND_LABEL || arg.type == OPERAND_FUNCTION)
        _output += "  lea " + reg + ", [rip + " + emit_operand(arg) + "]\n";
      else
        _output += "  mov " + reg + ", " + emit_value(arg) + "\n";
    }

    if (name.type == OPERAND_FUNCTION)
      _output += "  call " + emit_operand(name) + "\n";
    else {
      _output += "  mov r10, " + emit_value(name) + "\n";
      _output += "  call r10\n";
    }

    if (dst.type == OPERAND_VARIABLE && _current_function != nullptr) {
      size_t size = variable_stack_size(*_current_function, dst.name);
      if (size > 8) {
        size_t loc = _stack_loc.at(dst.name);
        _output += "  mov [rsp + " + std::to_string(loc) + "], rax\n";
        _output += "  mov [rsp + " + std::to_string(loc + 8) + "], rdx\n";
      } else {
        store_scratch(dst, "rax");
      }
    } else {
      store_scratch(dst, "rax");
    }
  } break;

  case OP_RET: {
    if (instr.srcs.size() == 3 &&
        instr.srcs[0].type == OPERAND_CONSTANT_INT) {
      auto cmp_op = static_cast<Opcode>(instr.srcs[0].int_value);
      const char *set = nullptr;
      switch (cmp_op) {
      case OP_CMP_EQ:  set = "sete"; break;
      case OP_CMP_NEQ: set = "setne"; break;
      case OP_CMP_LT:  set = "setl"; break;
      case OP_CMP_LTE: set = "setle"; break;
      case OP_CMP_GT:  set = "setg"; break;
      case OP_CMP_GTE: set = "setge"; break;
      default: break;
      }
      _output += "  mov r10, " + emit_value(instr.srcs[1]) + "\n";
      _output += "  cmp r10, " + emit_value(instr.srcs[2]) + "\n";
      _output += std::format("  {} al\n", set);
      _output += "  movzx rax, al\n";
    } else if (!instr.srcs.empty()) {
      const Operand &src = instr.srcs[0];
      if (src.type == OPERAND_VARIABLE && _current_function != nullptr) {
        size_t size = variable_stack_size(*_current_function, src.name);
        size_t loc = _stack_loc.at(src.name);
        _output += "  mov rax, [rsp + " + std::to_string(loc) + "]\n";
        if (size > 8)
          _output += "  mov rdx, [rsp + " + std::to_string(loc + 8) + "]\n";
      } else if (src.type == OPERAND_VARIABLE) {
        _output += "  mov rax, [rsp + " + emit_operand(src) + "]\n";
      } else {
        load_into("rax", src);
      }
    }
    _output += "  jmp " + _end_label + "\n";
  } break;
  }
}

std::string X86_64LinuxGasEmitter::emit_value(Operand operand) {
  switch (operand.type) {
  case OPERAND_CONSTANT_INT: return std::format("{}", operand.int_value);
  case OPERAND_CONSTANT:     return emit_value(*get_constant(operand.name));
  case OPERAND_STACK_ADDR:
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
  case OPERAND_STACK_ADDR:
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
