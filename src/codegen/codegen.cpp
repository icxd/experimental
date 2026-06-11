#include <codegen/codegen.hpp>
#include <codegen/emitters/aarch64_linux_gas.hpp>
#include <codegen/emitters/aarch64_macos_gas.hpp>
#include <codegen/emitters/x86_64_linux_gas.hpp>

Emitter *Emitter::get_emitter(Target target,
                              const std::vector<Constant> &constants,
                              const std::vector<Function *> &functions) {
  if (!target.is_valid_target())
    throw std::runtime_error("invalid target");

  if (target == TARGET_X86_64_MACOS_NASM)
    throw std::runtime_error("target not supported yet");
  else if (target == TARGET_X86_64_MACOS_GAS)
    throw std::runtime_error("target not supported yet");
  else if (target == TARGET_X86_64_LINUX_NASM)
    throw std::runtime_error("target not supported yet");
  else if (target == TARGET_X86_64_LINUX_GAS)
    return new X86_64LinuxGasEmitter(constants, functions);
  else if (target == TARGET_AARCH64_MACOS_GAS)
    return new Aarch64MacosGasEmitter(constants, functions);
  else if (target == TARGET_AARCH64_LINUX_GAS)
    return new Aarch64LinuxGasEmitter(constants, functions);

  throw std::runtime_error("invalid target");
}
