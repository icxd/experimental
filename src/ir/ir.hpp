#pragma once

#include <common.hpp>
#include <parser/ast.hpp>

#ifdef IR_PRINT_COLORS
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
  OPERAND_VARIABLE,
  OPERAND_TEMPORARY,
  OPERAND_FUNCTION,
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
  static Operand Variable(const std::string &n) {
    return Operand{OPERAND_VARIABLE, n};
  }
  static Operand Temporary(const std::string &n) {
    return Operand{OPERAND_TEMPORARY, n};
  }
  static Operand Function(const std::string &n) {
    return Operand{OPERAND_FUNCTION, n};
  }

  std::string to_string() const {
    switch (type) {
    case OPERAND_VARIABLE:  return ANSI_MAGENTA + name + ANSI_RESET;
    case OPERAND_TEMPORARY: return ANSI_CYAN + name + ANSI_RESET;
    case OPERAND_FUNCTION:  return ANSI_GREEN + name + ANSI_RESET;
    case OPERAND_CONSTANT_INT:
      return std::format(ANSI_YELLOW "{}" ANSI_RESET, int_value);
    }
  }
};

enum Opcode {
  OP_NOP, // nop
  OP_ASSIGN, // a = b
  OP_ADDROF, // a = &b
  OP_DEREF, // a = *b
  OP_ADD, // a = b + c
  OP_SUB, // a = b - c
  OP_MUL, // a = b * c
  OP_DIV, // a = b / c
  OP_CALL, // a = b(...)
  OP_RET, // ret a
};

struct Instruction {
  Opcode opcode;
  std::optional<Operand> dst;
  std::vector<Operand> srcs;

  std::string to_string() const {
    switch (opcode) {
    case OP_ASSIGN:
      return dst->to_string() + ANSI_RED " = " ANSI_RESET + srcs[0].to_string();
    case OP_ADDROF:
      return dst->to_string() + ANSI_RED " = &" ANSI_RESET +
             srcs[0].to_string();
    case OP_DEREF:
      return dst->to_string() + ANSI_RED " = *" ANSI_RESET +
             srcs[0].to_string();
    case OP_ADD:
      return dst->to_string() + ANSI_RED " = " ANSI_RESET +
             srcs[0].to_string() + ANSI_RED " + " ANSI_RESET +
             srcs[1].to_string();
    case OP_SUB:
      return dst->to_string() + ANSI_RED " = " ANSI_RESET +
             srcs[0].to_string() + ANSI_RED " - " ANSI_RESET +
             srcs[1].to_string();
    case OP_MUL:
      return dst->to_string() + ANSI_RED " = " ANSI_RESET +
             srcs[0].to_string() + ANSI_RED " * " ANSI_RESET +
             srcs[1].to_string();
    case OP_DIV:
      return dst->to_string() + ANSI_RED " = " ANSI_RESET +
             srcs[0].to_string() + ANSI_RED " / " ANSI_RESET +
             srcs[1].to_string();
    case OP_CALL: {
      std::string out = dst->to_string() + ANSI_RED " = " ANSI_RESET +
                        srcs[0].to_string() + ANSI_RED "(";
      for (size_t i = 1; i < srcs.size(); i++) {
        out += srcs[i].to_string();
        if (i + 1 < srcs.size())
          out += ANSI_RED ", ";
      }
      out += ANSI_RED ")";
      return out;
    }
    case OP_RET: return ANSI_RED "ret " ANSI_RESET + srcs[0].to_string();
    case OP_NOP: return ANSI_RED "nop" ANSI_RESET;
    }
  }
};

struct Function {
  std::string_view name;
  std::vector<std::string_view> parameters;
  std::vector<Instruction> instructions;
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
  void call(Operand dst, Operand name, std::vector<Operand> args = {}) {
    std::vector<Operand> srcs{};
    srcs.push_back(name);
    srcs.insert(srcs.end(), args.begin(), args.end());
    append(Instruction(OP_CALL, dst, srcs));
  }
  void ret() { append(Instruction(OP_RET, std::nullopt, {})); }
  void ret(Operand src) { append(Instruction(OP_RET, std::nullopt, {src})); }
};

class Builder {
public:
  Function *create_function(std::string_view name,
                            std::vector<std::string_view> parameters);
  Operand new_temp();

  const std::vector<Function *> &functions() const { return _functions; }
  void print() const;

private:
  std::vector<Function *> _functions{};
  size_t _temp_counter = 0;
};

std::vector<Instruction> fold_constants(const std::vector<Instruction> &input);
std::vector<Instruction>
fold_temporaries(const std::vector<Instruction> &input);
