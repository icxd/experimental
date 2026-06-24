#pragma once

#include <set>
#include <utility>

#include <common.hpp>
#include <parser/ast.hpp>

#ifndef IR_PRINT_COLORS
#define ANSI_BLACK "\033[30m"
#define ANSI_RED "\033[31m"
#define ANSI_GREEN "\033[32m"
#define ANSI_YELLOW "\033[33m"
#define ANSI_BLUE "\033[35m"
#define ANSI_MAGENTA "\033[34m"
#define ANSI_CYAN "\033[36m"
#define ANSI_WHITE "\033[37m"
#define ANSI_RESET "\033[0m"
#else
#define ANSI_BLACK ""
#define ANSI_RED ""
#define ANSI_GREEN ""
#define ANSI_YELLOW ""
#define ANSI_BLUE ""
#define ANSI_MAGENTA ""
#define ANSI_CYAN ""
#define ANSI_WHITE ""
#define ANSI_RESET ""
#endif

enum OperandType {
  OPERAND_CONSTANT_INT,
  OPERAND_CONSTANT,
  OPERAND_VARIABLE,
  OPERAND_STACK_ADDR,
  OPERAND_TEMPORARY,
  OPERAND_FUNCTION,
  OPERAND_LABEL,
};
struct Operand {
  OperandType type;
  std::string name;
  int64_t int_value = 0;

  static Operand ConstantInt(int64_t v) {
    Operand op;
    op.type = OPERAND_CONSTANT_INT;
    op.int_value = v;
    return op;
  }
  static Operand Constant(const std::string &n) {
    return Operand{OPERAND_CONSTANT, n};
  }
  static Operand Variable(const std::string &n) {
    return Operand{OPERAND_VARIABLE, n};
  }
  static Operand StackAddr(const std::string &n) {
    return Operand{OPERAND_STACK_ADDR, n};
  }
  static Operand Temporary(const std::string &n) {
    return Operand{OPERAND_TEMPORARY, n};
  }
  static Operand Function(const std::string &n) {
    return Operand{OPERAND_FUNCTION, n};
  }
  static Operand Label(const std::string &n) {
    return Operand{OPERAND_LABEL, n};
  }

  std::string debug() const {
    switch (type) {
    case OPERAND_VARIABLE:     return std::format("Variable({})", name);
    case OPERAND_STACK_ADDR:   return std::format("StackAddr({})", name);
    case OPERAND_CONSTANT:     return std::format("Constant({})", name);
    case OPERAND_TEMPORARY:    return std::format("Temporary({})", name);
    case OPERAND_FUNCTION:     return std::format("Function({})", name);
    case OPERAND_CONSTANT_INT: return std::format("ConstantInt({})", int_value);
    case OPERAND_LABEL:        return std::format("Label({})", name);
    }
    std::unreachable();
  }

  std::string to_string() const {
    switch (type) {
    case OPERAND_VARIABLE:
      return ANSI_MAGENTA + name + ANSI_RESET;
    case OPERAND_STACK_ADDR:
      return std::format(ANSI_MAGENTA "&{}" ANSI_RESET, name);
    case OPERAND_CONSTANT:  return ANSI_BLUE + name + ANSI_RESET;
    case OPERAND_TEMPORARY: return ANSI_CYAN + name + ANSI_RESET;
    case OPERAND_FUNCTION:  return ANSI_GREEN + name + ANSI_RESET;
    case OPERAND_CONSTANT_INT:
      return std::format(ANSI_YELLOW "{}" ANSI_RESET, int_value);
    case OPERAND_LABEL:     return ANSI_WHITE + name + ANSI_RESET;
    }
    std::unreachable();
  }
};

enum Opcode {
  OP_NOP, // nop
  OP_ASSIGN, // a = b
  OP_ADDROF, // a = &b
  OP_DEREF, // a = *b
  OP_LOAD_OFFSET, // a = *(b + offset)
  OP_STORE_OFFSET, // *(b + offset) = c
  OP_LOAD_BYTE, // a = *(byte *)b
  OP_STORE_BYTE, // *(byte *)b = c
  OP_LOAD_LABEL, // a = &label
  OP_ADD, // a = b + c
  OP_SUB, // a = b - c
  OP_MUL, // a = b * c
  OP_DIV, // a = b / c
  OP_CMP_EQ,
  OP_CMP_NEQ,
  OP_CMP_LT,
  OP_CMP_LTE,
  OP_CMP_GT,
  OP_CMP_GTE,
  OP_LABEL,
  OP_JMP,
  OP_JMP_IF_ZERO,
  OP_JMP_IF_NONZERO,
  OP_JMP_IF_EQ,
  OP_JMP_IF_NE,
  OP_CALL, // a = b(...)
  OP_RET, // ret a
};

static std::string get_cmp_op(Opcode opcode) {
  switch (opcode) {
  case OP_CMP_EQ: return "==";
  case OP_CMP_NEQ: return "!=";
  case OP_CMP_LT: return "<";
  case OP_CMP_LTE: return "<=";
  case OP_CMP_GT: return ">";
  case OP_CMP_GTE: return ">=";
  default: std::unreachable();
  }
}

struct Instruction {
  Opcode opcode;
  std::optional<Operand> dst;
  std::vector<Operand> srcs;

  std::string to_string() const {
    switch (opcode) {
    case OP_NOP: return "  " ANSI_RED "nop" ANSI_RESET;
    case OP_ASSIGN:
      return "  " + dst->to_string() + ANSI_RED " = " ANSI_RESET + srcs[0].to_string();
    case OP_ADDROF:
      return "  " + dst->to_string() + ANSI_RED " = &" ANSI_RESET +
             srcs[0].to_string();
    case OP_DEREF:
      return "  " + dst->to_string() + ANSI_RED " = *" ANSI_RESET +
             srcs[0].to_string();
    case OP_LOAD_OFFSET:
      return "  " + dst->to_string() + ANSI_RED " = *(" ANSI_RESET +
             srcs[0].to_string() + ANSI_RED " + " ANSI_RESET +
             srcs[1].to_string() + ANSI_RED ")\033[0m";
    case OP_STORE_OFFSET:
      return "  " ANSI_RED "*(" ANSI_RESET + srcs[0].to_string() + ANSI_RED " + " +
             ANSI_RESET + srcs[1].to_string() + ANSI_RED ") = " ANSI_RESET +
             srcs[2].to_string();
    case OP_LOAD_BYTE:
      return "  " + dst->to_string() + ANSI_RED " = *(byte *)" ANSI_RESET +
             srcs[0].to_string();
    case OP_STORE_BYTE:
      return "  " ANSI_RED "*(byte *)" ANSI_RESET + srcs[0].to_string() +
             ANSI_RED " = " ANSI_RESET + srcs[1].to_string();
    case OP_LOAD_LABEL:
      return "  " + dst->to_string() + ANSI_RED " = &" ANSI_RESET +
             srcs[0].to_string();
    case OP_ADD:
      return "  " + dst->to_string() + ANSI_RED " = " ANSI_RESET +
             srcs[0].to_string() + ANSI_RED " + " ANSI_RESET +
             srcs[1].to_string();
    case OP_SUB:
      return "  " + dst->to_string() + ANSI_RED " = " ANSI_RESET +
             srcs[0].to_string() + ANSI_RED " - " ANSI_RESET +
             srcs[1].to_string();
    case OP_MUL:
      return "  " + dst->to_string() + ANSI_RED " = " ANSI_RESET +
             srcs[0].to_string() + ANSI_RED " * " ANSI_RESET +
             srcs[1].to_string();
    case OP_DIV:
      return "  " + dst->to_string() + ANSI_RED " = " ANSI_RESET +
             srcs[0].to_string() + ANSI_RED " / " ANSI_RESET +
             srcs[1].to_string();
    case OP_CMP_EQ:
    case OP_CMP_NEQ:
    case OP_CMP_LT:
    case OP_CMP_LTE:
    case OP_CMP_GT:
    case OP_CMP_GTE:
      return "  " + dst->to_string() + ANSI_RED " = cmp " ANSI_RESET +
             srcs[0].to_string() + ANSI_RED " " + get_cmp_op(opcode) + " " ANSI_RESET +
             srcs[1].to_string();
    case OP_LABEL:      return srcs[0].to_string() + ":";
    case OP_JMP:        return "  " ANSI_RED "jmp " ANSI_RESET + srcs[0].to_string();
    case OP_JMP_IF_ZERO:
      return "  " ANSI_RED "jz " ANSI_RESET + srcs[0].to_string() + ANSI_RED " -> " +
             ANSI_RESET + srcs[1].to_string();
    case OP_JMP_IF_NONZERO:
      return "  " ANSI_RED "jnz " ANSI_RESET + srcs[0].to_string() + ANSI_RED " -> " +
             ANSI_RESET + srcs[1].to_string();
    case OP_JMP_IF_EQ:
      return "  " ANSI_RED "jeq " ANSI_RESET + srcs[0].to_string() + ANSI_RED ", " +
             ANSI_RESET + srcs[1].to_string() + ANSI_RED " -> " +
             ANSI_RESET + srcs[2].to_string();
    case OP_JMP_IF_NE:
      return "  " ANSI_RED "jne " ANSI_RESET + srcs[0].to_string() + ANSI_RED ", " +
             ANSI_RESET + srcs[1].to_string() + ANSI_RED " -> " +
             ANSI_RESET + srcs[2].to_string();
    case OP_CALL: {
      std::string out = dst->to_string() + ANSI_RED " = " ANSI_RESET +
                        srcs[0].to_string() + ANSI_RED "(";
      for (size_t i = 1; i < srcs.size(); i++) {
        out += srcs[i].to_string();
        if (i + 1 < srcs.size())
          out += ANSI_RED ", ";
      }
      out += ANSI_RED ")";
      return "  " + out;
    }
    case OP_RET: return "  " ANSI_RED "ret " ANSI_RESET + srcs[0].to_string();
    }
    std::unreachable();
  }
};

struct Function {
  std::string_view name;
  std::vector<std::string_view> parameters;
  std::vector<Instruction> instructions;
  std::map<std::string, size_t> variable_sizes;
  std::set<std::string> indirect_vars;
  std::set<std::string> struct_value_params;
  Linkage linkage = LINK_INTERN;

  void print() const {
    if (linkage == LINK_EXTERN)
      std::cout << ANSI_RED << "extern ";
    std::cout << ANSI_RED << "def " << ANSI_GREEN << name << ANSI_RED << "("
              << ANSI_RESET;
    for (size_t i = 0; i < parameters.size(); i++) {
      std::cout << ANSI_MAGENTA << parameters[i] << ANSI_RESET;
      if (i + 1 < parameters.size())
        std::cout << ANSI_RED << ", " << ANSI_RESET;
    }
    std::cout << ANSI_RED << ")";
    if (linkage == LINK_INTERN)
      std::cout << ":";
    std::cout << "\n" << ANSI_RESET;
    for (const auto &instr: instructions) {
      std::cout << "  " << instr.to_string() << "\n";
    }
  }

  void append(Instruction instr) { instructions.push_back(instr); }

public:
  void nop() { append(Instruction(OP_NOP)); }
  void assign(Operand dst, Operand src) {
    append(Instruction(OP_ASSIGN, dst, {src}));
  }
  void addrof(Operand dst, Operand src) {
    append(Instruction(OP_ADDROF, dst, {src}));
  }
  void deref(Operand dst, Operand src) {
    append(Instruction(OP_DEREF, dst, {src}));
  }
  void load_offset(Operand dst, Operand base, int64_t offset) {
    append(Instruction(OP_LOAD_OFFSET, dst,
                       {base, Operand::ConstantInt(offset)}));
  }
  void store_offset(Operand base, int64_t offset, Operand value) {
    append(Instruction(OP_STORE_OFFSET, std::nullopt,
                       {base, Operand::ConstantInt(offset), value}));
  }
  void load_byte(Operand dst, Operand addr) {
    append(Instruction(OP_LOAD_BYTE, dst, {addr}));
  }
  void store_byte(Operand addr, Operand value) {
    append(Instruction(OP_STORE_BYTE, std::nullopt, {addr, value}));
  }
  void load_label(Operand dst, Operand label) {
    append(Instruction(OP_LOAD_LABEL, dst, {label}));
  }
  void add(Operand dst, Operand src1, Operand src2) {
    append(Instruction(OP_ADD, dst, {src1, src2}));
  }
  void sub(Operand dst, Operand src1, Operand src2) {
    append(Instruction(OP_SUB, dst, {src1, src2}));
  }
  void mul(Operand dst, Operand src1, Operand src2) {
    append(Instruction(OP_MUL, dst, {src1, src2}));
  }
  void div(Operand dst, Operand src1, Operand src2) {
    append(Instruction(OP_DIV, dst, {src1, src2}));
  }
  void cmp_eq(Operand dst, Operand src1, Operand src2) {
    append(Instruction(OP_CMP_EQ, dst, {src1, src2}));
  }
  void cmp_neq(Operand dst, Operand src1, Operand src2) {
    append(Instruction(OP_CMP_NEQ, dst, {src1, src2}));
  }
  void cmp_lt(Operand dst, Operand src1, Operand src2) {
    append(Instruction(OP_CMP_LT, dst, {src1, src2}));
  }
  void cmp_lte(Operand dst, Operand src1, Operand src2) {
    append(Instruction(OP_CMP_LTE, dst, {src1, src2}));
  }
  void cmp_gt(Operand dst, Operand src1, Operand src2) {
    append(Instruction(OP_CMP_GT, dst, {src1, src2}));
  }
  void cmp_gte(Operand dst, Operand src1, Operand src2) {
    append(Instruction(OP_CMP_GTE, dst, {src1, src2}));
  }
  void label(Operand lbl) { append(Instruction(OP_LABEL, std::nullopt, {lbl})); }
  void jmp(Operand lbl) { append(Instruction(OP_JMP, std::nullopt, {lbl})); }
  void jmp_if_zero(Operand cond, Operand lbl) {
    append(Instruction(OP_JMP_IF_ZERO, std::nullopt, {cond, lbl}));
  }
  void jmp_if_nonzero(Operand cond, Operand lbl) {
    append(Instruction(OP_JMP_IF_NONZERO, std::nullopt, {cond, lbl}));
  }
  void call(Operand dst, Operand name, std::vector<Operand> args = {}) {
    std::vector<Operand> srcs{};
    srcs.push_back(name);
    srcs.insert(srcs.end(), args.begin(), args.end());
    append(Instruction(OP_CALL, dst, srcs));
  }
  void ret() { append(Instruction(OP_RET, std::nullopt, {})); }
  void ret(Operand src) { append(Instruction(OP_RET, std::nullopt, {src})); }
};

struct Constant {
  std::string_view name;
  Operand value;

  Constant(std::string_view n, Operand v) : name(n), value(v) {}

  std::string to_string() const {
    return std::format("{} = {}", ANSI_MAGENTA + std::string(name) + ANSI_RESET,
                       value.to_string());
  }
};

struct RodataEntry {
  std::string label;
  std::string bytes;
};

class Builder {
public:
  Function *create_function(std::string_view name,
                            std::vector<std::string_view> parameters);
  Operand new_temp();
  std::string new_label() { return std::format(".L{}", _label_counter++); }
  void reset_labels() { _label_counter = 0; }

  void create_constant(std::string_view name, Operand value) {
    _constants.push_back(Constant{name, value});
  }

  std::string create_rodata(std::string bytes) {
    std::string label = std::format(".Lstr{}", _rodata_counter++);
    _rodata.push_back(RodataEntry{label, std::move(bytes)});
    return label;
  }

  const std::vector<Function *> &functions() const { return _functions; }

  const std::vector<Constant> &constants() const { return _constants; }
  std::vector<Constant> &constants() { return _constants; }
  const std::vector<RodataEntry> &rodata() const { return _rodata; }

  void print() const;

  std::optional<Constant> find_constant(std::string_view name) const {
    for (const auto &constant: _constants) {
      if (constant.name == name)
        return constant;
    }
    return std::nullopt;
  }

private:
  std::vector<Function *> _functions{};
  size_t _temp_counter = 0;
  size_t _label_counter = 0;
  size_t _rodata_counter = 0;
  std::vector<Constant> _constants{};
  std::vector<RodataEntry> _rodata{};
};

std::vector<Instruction> fold_constants(const std::vector<Instruction> &input);
std::vector<Instruction>
copy_propagate(const std::vector<Instruction> &input);
std::vector<Instruction>
fold_branches(const std::vector<Instruction> &input);
std::vector<Instruction>
fold_temporaries(const std::vector<Instruction> &input);
std::vector<Instruction> global_cse(const std::vector<Instruction> &input);
std::vector<Instruction>
store_load_forward(const std::vector<Instruction> &input);
std::vector<Instruction>
fuse_cmp_branch(const std::vector<Instruction> &input);
std::vector<Instruction>
eliminate_copy_assigns(const std::vector<Instruction> &input);
std::vector<Instruction>
simplify_cfg(const std::vector<Instruction> &input);
std::vector<Instruction>
dead_code_elim(const std::vector<Instruction> &input);
void optimize_function(Function &fn);
