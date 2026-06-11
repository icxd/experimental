#pragma once

#include <codegen/codegen.hpp>
#include <common.hpp>

class X86_64LinuxGasEmitter : public Emitter {
public:
  X86_64LinuxGasEmitter(const std::vector<Constant> &constants,
                        const std::vector<Function *> &functions) :
      _constants(constants), _functions(functions) {}

  void emit() override;
  const std::string &output() const override { return _output; }

private:
  void emit_function(Function function) override;
  void emit_instruction(Instruction instr) override;
  std::string emit_operand(Operand operand) override;

  const std::map<std::string, std::string> &current_regmap() const {
    return _register_maps.at(_current_fn);
  }

  std::optional<Operand> get_constant(std::string name) const {
    for (const auto &constant: _constants) {
      if (constant.name == name)
        return constant.value;
    }
    return std::nullopt;
  }

  std::vector<Constant> _constants;
  std::vector<Function *> _functions;

  std::string _output;
  std::string _current_fn = "";
  std::map<std::string, size_t> _stack_loc{};
  size_t _next_stack_loc = 16;
  std::map<std::string, std::map<std::string, std::string>> _register_maps;
  std::vector<std::string> _registers{"r10", "r11", "r12", "r13",
                                      "r14", "r15", "rbx", "rax"};
};
