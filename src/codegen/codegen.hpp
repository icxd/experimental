#pragma once

#include <common.hpp>
#include <ir/ir.hpp>

enum Architecture { ARCH_X86_64, ARCH_AARCH64 };
enum OSType { OS_MACOS, OS_LINUX };
enum Assembler { ASM_NASM, ASM_GAS };

struct Target {
  Architecture arch;
  OSType os_type;
  Assembler assembler;

  constexpr bool operator==(const Target &other) const {
    return arch == other.arch && os_type == other.os_type &&
           assembler == other.assembler;
  }

  bool is_valid_target() const {
    struct Combo {
      Architecture arch;
      OSType os;
      Assembler asm_;
    };

    static constexpr Combo valid_combinations[] = {
        {ARCH_X86_64, OS_MACOS, ASM_NASM}, {ARCH_X86_64, OS_MACOS, ASM_GAS},
        {ARCH_X86_64, OS_LINUX, ASM_NASM}, {ARCH_X86_64, OS_LINUX, ASM_GAS},
        {ARCH_AARCH64, OS_MACOS, ASM_GAS}, {ARCH_AARCH64, OS_LINUX, ASM_GAS},
    };

    for (const auto &combo: valid_combinations) {
      if (arch == combo.arch && os_type == combo.os && assembler == combo.asm_)
        return true;
    }

    return false;
  }
};

constexpr Target TARGET_X86_64_MACOS_NASM = {ARCH_X86_64, OS_MACOS, ASM_NASM},
                 TARGET_X86_64_MACOS_GAS = {ARCH_X86_64, OS_MACOS, ASM_GAS},
                 TARGET_X86_64_LINUX_NASM = {ARCH_X86_64, OS_LINUX, ASM_NASM},
                 TARGET_X86_64_LINUX_GAS = {ARCH_X86_64, OS_LINUX, ASM_GAS},
                 TARGET_AARCH64_MACOS_GAS = {ARCH_AARCH64, OS_MACOS, ASM_GAS},
                 TARGET_AARCH64_LINUX_GAS = {ARCH_AARCH64, OS_LINUX, ASM_GAS};

class Emitter {
public:
  static Emitter *get_emitter(Target target);

  virtual void emit(const std::vector<Function *> &functions) = 0;

private:
  virtual void emit_function(Function function) = 0;
  virtual void emit_instruction(Instruction instr) = 0;
  virtual std::string emit_operand(Operand operand) = 0;
};
