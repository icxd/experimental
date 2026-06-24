#include <utility>

#include <codegen/emitters/aarch64_linux_gas.hpp>
#include <codegen/regalloc.hpp>
#include <ir/ir.hpp>

static size_t variable_stack_size(const Function &function,
                                  std::string_view name) {
  auto it = function.variable_sizes.find(std::string(name));
  if (it != function.variable_sizes.end())
    return it->second;
  return 8;
}

static size_t align_stack(size_t size) {
  return (size + 15) & ~size_t(15);
}

static bool var_is_indirect(const Function *fn, std::string_view name) {
  return fn != nullptr && fn->indirect_vars.contains(std::string(name));
}

static bool var_is_struct_value(const Function *fn, std::string_view name) {
  return fn != nullptr && fn->struct_value_params.contains(std::string(name));
}

static void emit_struct_param_copy(std::string &output, const std::string &src_reg,
                                   size_t dest_off, size_t size) {
  output += "  mov x9, " + src_reg + "\n";
  for (size_t off = 0; off < size; off += 8) {
    output += "  ldr x10, [x9, #" + std::to_string(off) + "]\n";
    output += "  str x10, [sp, #" + std::to_string(dest_off + off) + "]\n";
  }
}

static size_t compute_locals_size(const Function &function) {
  std::set<std::string> local_vars;
  for (const auto &instr: function.instructions) {
    if (instr.dst.has_value() && instr.dst->type == OPERAND_VARIABLE)
      local_vars.insert(instr.dst->name);
    for (const auto &src: instr.srcs) {
      if (src.type == OPERAND_VARIABLE)
        local_vars.insert(src.name);
    }
  }

  size_t total = 0;
  for (const auto &name: local_vars)
    total += variable_stack_size(function, name);
  return total;
}

static void emit_rodata_section(std::string &output,
                                const std::vector<RodataEntry> &rodata) {
  if (rodata.empty())
    return;
  output += ".section .rodata\n";
  for (const auto &entry: rodata) {
    output += entry.label + ":\n";
    output += "  .asciz \"";
    for (unsigned char ch: entry.bytes) {
      if (ch == '\n')
        output += "\\n";
      else if (ch == '\t')
        output += "\\t";
      else if (ch == '\r')
        output += "\\r";
      else if (ch == '\\')
        output += "\\\\";
      else if (ch == '"')
        output += "\\\"";
      else if (ch == '\0')
        break;
      else if (std::isprint(ch))
        output += static_cast<char>(ch);
      else
        output += std::format("\\{:03o}", ch);
    }
    output += "\"\n";
  }
}

void Aarch64LinuxGasEmitter::emit() {
  _output += ".text\n";
  for (const auto *function: _functions)
    emit_function(*function);
  emit_rodata_section(_output, _rodata);
  _output += ".section .note.GNU-stack,\"\",@progbits\n";
}

void Aarch64LinuxGasEmitter::emit_function(Function function) {
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

    auto layout_locals = [&]() {
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
          if (src.type == OPERAND_VARIABLE)
            layout_var(src.name);
        }
      }
    };

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

    layout_locals();

    size_t stack_size = align_stack(_next_stack_loc);
    if (stack_size < 16)
      stack_size = 16;

    if (has_call_instruction) {
      _output += "  stp x29, x30, [sp, -" + std::to_string(stack_size) + "]!\n";
      _output += "  mov x29, sp\n";
    } else if (local_vars > 0) {
      _output += "  sub sp, sp, " + std::to_string(stack_size) + "\n";
    }

    _next_stack_loc = 16;
    _stack_loc.clear();
    _var_sizes.clear();

    for (size_t i = 0; i < function.parameters.size(); i++) {
      std::string param(function.parameters[i]);
      size_t stack_off = _next_stack_loc;
      _stack_loc.insert({param, stack_off});
      if (var_is_struct_value(&function, param)) {
        emit_struct_param_copy(_output, "x" + std::to_string(i), stack_off,
                               variable_stack_size(function, param));
      } else {
        _output += "  str x" + std::to_string(i) + ", [sp, #" +
                   std::to_string(stack_off) + "]\n";
      }
      _next_stack_loc += variable_stack_size(function, param);
    }

    for (const auto &[temp, alloc]: regmap) {
      if (alloc.spilled) {
        _spill_loc[temp] = _next_stack_loc;
        _next_stack_loc += 8;
      }
    }

    layout_locals();

    _end_label = ".L_end_" + name;
    for (const auto &instr: function.instructions)
      emit_instruction(instr);

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

void Aarch64LinuxGasEmitter::emit_instruction(Instruction instr) {
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
      _output += "  add " + reg + ", sp, #" + emit_operand(src) + "\n";
      return;
    }
    if (src.type == OPERAND_LABEL || src.type == OPERAND_FUNCTION) {
      _output += "  adrp " + reg + ", " + emit_operand(src) + "@PAGE\n";
      _output += "  add " + reg + ", " + reg + ", " + emit_operand(src) +
                 "@PAGEOFF\n";
      return;
    }
    if (src.type == OPERAND_VARIABLE || src.type == OPERAND_TEMPORARY)
      _output += "  ldr " + reg + ", " + emit_value(src) + "\n";
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

    if (src.type == OPERAND_VARIABLE || src.type == OPERAND_TEMPORARY)
      _output += "  ldr x9, " + emit_value(src) + "\n";
    else
      _output += "  mov x9, " + emit_value(src) + "\n";
    store_scratch(dst, "x9");
  } break;

  case OP_ADDROF: {
    assert(instr.dst.has_value());
    Operand dst = *instr.dst;
    assert(instr.srcs.size() == 1);
    Operand src = instr.srcs[0];

    _output += "  add x9, sp, #" + emit_operand(src) + "\n";

    store_scratch(dst, "x9");
  } break;

  case OP_DEREF: {
    assert(instr.dst.has_value());
    Operand dst = *instr.dst;
    assert(instr.srcs.size() == 1);
    Operand src = instr.srcs[0];

    if (src.type == OPERAND_VARIABLE)
      _output += "  ldr x9, [sp, #" + emit_operand(src) + "]\n";
    else if (src.type == OPERAND_TEMPORARY)
      _output += "  ldr x9, " + emit_value(src) + "\n";
    else
      _output += "  mov x9, " + emit_value(src) + "\n";
    _output += "  ldr x9, [x9]\n";
    store_scratch(dst, "x9");
  } break;

  case OP_LOAD_OFFSET: {
    assert(instr.dst.has_value());
    Operand dst = *instr.dst;
    Operand base = instr.srcs[0];
    int64_t offset = instr.srcs[1].int_value;
    if (base.type == OPERAND_VARIABLE &&
        !var_is_indirect(_current_function, base.name)) {
      size_t base_off = _stack_loc.at(base.name);
      _output += "  ldr x9, [sp, #" + std::to_string(base_off + offset) +
                 "]\n";
    } else {
      if (base.type == OPERAND_VARIABLE) {
        size_t base_off = _stack_loc.at(base.name);
        _output += "  ldr x10, [sp, #" + std::to_string(base_off) + "]\n";
      } else {
        load_into("x10", base);
      }
      if (offset != 0)
        _output += "  add x10, x10, #" + std::to_string(offset) + "\n";
      _output += "  ldr x9, [x10]\n";
    }
    store_scratch(dst, "x9");
  } break;

  case OP_STORE_OFFSET: {
    Operand base = instr.srcs[0];
    int64_t offset = instr.srcs[1].int_value;
    Operand value = instr.srcs[2];
    load_into("x11", value);
    if (base.type == OPERAND_VARIABLE &&
        !var_is_indirect(_current_function, base.name)) {
      size_t base_off = _stack_loc.at(base.name);
      _output += "  str x11, [sp, #" + std::to_string(base_off + offset) +
                 "]\n";
    } else {
      if (base.type == OPERAND_VARIABLE) {
        size_t base_off = _stack_loc.at(base.name);
        _output += "  ldr x10, [sp, #" + std::to_string(base_off) + "]\n";
      } else {
        load_into("x10", base);
      }
      if (offset != 0)
        _output += "  add x10, x10, #" + std::to_string(offset) + "\n";
      _output += "  str x11, [x10]\n";
    }
  } break;

  case OP_LOAD_BYTE: {
    assert(instr.dst.has_value());
    Operand dst = *instr.dst;
    assert(instr.srcs.size() == 1);
    load_into("x10", instr.srcs[0]);
    _output += "  ldrb w9, [x10]\n";
    store_scratch(dst, "x9");
  } break;

  case OP_STORE_BYTE: {
    assert(instr.srcs.size() == 2);
    load_into("x11", instr.srcs[1]);
    load_into("x10", instr.srcs[0]);
    _output += "  strb w11, [x10]\n";
  } break;

  case OP_LOAD_LABEL: {
    assert(instr.dst.has_value());
    Operand dst = *instr.dst;
    _output += "  adrp x9, " + emit_operand(instr.srcs[0]) + "\n";
    _output += "  add x9, x9, :lo12:" + emit_operand(instr.srcs[0]) + "\n";
    store_scratch(dst, "x9");
  } break;

  case OP_ADD: {
    assert(instr.dst.has_value());
    Operand dst = *instr.dst;
    assert(instr.srcs.size() == 2);
    Operand src1 = instr.srcs[0];
    Operand src2 = instr.srcs[1];

    if (src1.type == OPERAND_VARIABLE || src1.type == OPERAND_TEMPORARY)
      _output += "  ldr x10, " + emit_value(src1) + "\n";
    else
      _output += "  mov x10, " + emit_value(src1) + "\n";
    if (src2.type == OPERAND_VARIABLE || src2.type == OPERAND_TEMPORARY)
      _output += "  ldr x11, " + emit_value(src2) + "\n";
    else
      _output += "  mov x11, " + emit_value(src2) + "\n";
    _output += "  add x9, x10, x11\n";
    store_scratch(dst, "x9");
  } break;

  case OP_SUB: {
    assert(instr.dst.has_value());
    Operand dst = *instr.dst;
    Operand src1 = instr.srcs[0];
    Operand src2 = instr.srcs[1];
    if (src1.type == OPERAND_VARIABLE || src1.type == OPERAND_TEMPORARY)
      _output += "  ldr x10, " + emit_value(src1) + "\n";
    else
      _output += "  mov x10, " + emit_value(src1) + "\n";
    if (src2.type == OPERAND_VARIABLE || src2.type == OPERAND_TEMPORARY)
      _output += "  ldr x11, " + emit_value(src2) + "\n";
    else
      _output += "  mov x11, " + emit_value(src2) + "\n";
    _output += "  sub x9, x10, x11\n";
    store_scratch(dst, "x9");
  } break;

  case OP_MUL: {
    assert(instr.dst.has_value());
    Operand dst = *instr.dst;
    Operand src1 = instr.srcs[0];
    Operand src2 = instr.srcs[1];
    if (src1.type == OPERAND_VARIABLE || src1.type == OPERAND_TEMPORARY)
      _output += "  ldr x10, " + emit_value(src1) + "\n";
    else
      _output += "  mov x10, " + emit_value(src1) + "\n";
    if (src2.type == OPERAND_VARIABLE || src2.type == OPERAND_TEMPORARY)
      _output += "  ldr x11, " + emit_value(src2) + "\n";
    else
      _output += "  mov x11, " + emit_value(src2) + "\n";
    _output += "  mul x9, x10, x11\n";
    store_scratch(dst, "x9");
  } break;

  case OP_DIV: {
    assert(instr.dst.has_value());
    Operand dst = *instr.dst;
    Operand src1 = instr.srcs[0];
    Operand src2 = instr.srcs[1];
    if (src1.type == OPERAND_VARIABLE || src1.type == OPERAND_TEMPORARY)
      _output += "  ldr x10, " + emit_value(src1) + "\n";
    else
      _output += "  mov x10, " + emit_value(src1) + "\n";
    if (src2.type == OPERAND_VARIABLE || src2.type == OPERAND_TEMPORARY)
      _output += "  ldr x11, " + emit_value(src2) + "\n";
    else
      _output += "  mov x11, " + emit_value(src2) + "\n";
    _output += "  sdiv x9, x10, x11\n";
    store_scratch(dst, "x9");
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
    if (src1.type == OPERAND_VARIABLE || src1.type == OPERAND_TEMPORARY)
      _output += "  ldr x10, " + emit_value(src1) + "\n";
    else
      _output += "  mov x10, " + emit_value(src1) + "\n";
    if (src2.type == OPERAND_VARIABLE || src2.type == OPERAND_TEMPORARY)
      _output += "  ldr x11, " + emit_value(src2) + "\n";
    else
      _output += "  mov x11, " + emit_value(src2) + "\n";
    _output += "  cmp x10, x11\n";
    _output += std::format("  cset x9, {}\n", cond);
    store_scratch(dst, "x9");
  } break;

  case OP_LABEL: {
    _output += emit_operand(instr.srcs[0]) + ":\n";
  } break;

  case OP_JMP: {
    _output += "  b " + emit_operand(instr.srcs[0]) + "\n";
  } break;

  case OP_JMP_IF_ZERO: {
    Operand cond = instr.srcs[0];
    if (cond.type == OPERAND_VARIABLE || cond.type == OPERAND_TEMPORARY)
      _output += "  ldr x9, " + emit_value(cond) + "\n";
    else
      _output += "  mov x9, " + emit_value(cond) + "\n";
    _output += "  cbz x9, " + emit_operand(instr.srcs[1]) + "\n";
  } break;

  case OP_JMP_IF_NONZERO: {
    load_into("x9", instr.srcs[0]);
    _output += "  cbnz x9, " + emit_operand(instr.srcs[1]) + "\n";
  } break;

  case OP_JMP_IF_EQ: {
    load_into("x10", instr.srcs[0]);
    load_into("x11", instr.srcs[1]);
    _output += "  cmp x10, x11\n";
    _output += "  b.eq " + emit_operand(instr.srcs[2]) + "\n";
  } break;

  case OP_JMP_IF_NE: {
    load_into("x10", instr.srcs[0]);
    load_into("x11", instr.srcs[1]);
    _output += "  cmp x10, x11\n";
    _output += "  b.ne " + emit_operand(instr.srcs[2]) + "\n";
  } break;

  case OP_CALL: {
    assert(instr.dst.has_value());
    Operand dst = *instr.dst;
    assert(instr.srcs.size() >= 1);
    Operand name = instr.srcs[0];

    for (size_t i = 1; i < instr.srcs.size(); i++)
      load_into("x" + std::to_string(i - 1), instr.srcs[i]);
    if (name.type == OPERAND_FUNCTION)
      _output += "  bl " + emit_operand(name) + "\n";
    else {
      load_into("x16", name);
      _output += "  blr x16\n";
    }

    store_scratch(dst, "x0");
  } break;

  case OP_RET: {
    if (instr.srcs.size() == 3 &&
        instr.srcs[0].type == OPERAND_CONSTANT_INT) {
      auto cmp_op = static_cast<Opcode>(instr.srcs[0].int_value);
      const char *cond = nullptr;
      switch (cmp_op) {
      case OP_CMP_EQ:  cond = "eq"; break;
      case OP_CMP_NEQ: cond = "ne"; break;
      case OP_CMP_LT:  cond = "lt"; break;
      case OP_CMP_LTE: cond = "le"; break;
      case OP_CMP_GT:  cond = "gt"; break;
      case OP_CMP_GTE: cond = "ge"; break;
      default: break;
      }
      load_into("x10", instr.srcs[1]);
      load_into("x11", instr.srcs[2]);
      _output += "  cmp x10, x11\n";
      _output += std::format("  cset x0, {}\n", cond);
    } else if (!instr.srcs.empty()) {
      load_into("x0", instr.srcs[0]);
    }
    _output += "  b " + _end_label + "\n";
  } break;
  }
}

std::string Aarch64LinuxGasEmitter::emit_value(Operand operand) {
  switch (operand.type) {
  case OPERAND_CONSTANT_INT: return std::format("#{}", operand.int_value);
  case OPERAND_CONSTANT:     return emit_value(*get_constant(operand.name));
  case OPERAND_STACK_ADDR:
  case OPERAND_VARIABLE:
    return "[sp, #" + std::to_string(_stack_loc.at(operand.name)) + "]";
  case OPERAND_TEMPORARY: {
    const auto &alloc = current_alloc().at(operand.name);
    if (alloc.spilled)
      return "[sp, #" + std::to_string(_spill_loc.at(operand.name)) + "]";
    return alloc.reg;
  }
  default: PANIC("unexpected operand in value position");
  }
}

void Aarch64LinuxGasEmitter::store_scratch(const Operand &dst,
                                           const std::string &scratch) {
  if (dst.type == OPERAND_VARIABLE)
    _output += "  str " + scratch + ", [sp, #" + emit_operand(dst) + "]\n";
  else if (dst.type == OPERAND_TEMPORARY) {
    const auto &alloc = current_alloc().at(dst.name);
    if (alloc.spilled)
      _output += "  str " + scratch + ", [sp, #" +
                 std::to_string(_spill_loc.at(dst.name)) + "]\n";
    else
      _output += "  mov " + alloc.reg + ", " + scratch + "\n";
  }
}

std::string Aarch64LinuxGasEmitter::emit_operand(Operand operand) {
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
