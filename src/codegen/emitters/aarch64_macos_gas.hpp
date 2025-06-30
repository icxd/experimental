#pragma once

#include <codegen/codegen.hpp>

class Aarch64MacosGasEmitter : public Emitter {
public:
  Aarch64MacosGasEmitter() = default;

  void emit(const std::vector<Function *> &functions) override;

  const std::string &output() const { return _output; }

private:
  void emit_function(Function function) override;
  void emit_instruction(Instruction instr) override;
  std::string emit_operand(Operand operand) override;

  const std::map<std::string, std::string> &current_regmap() const {
    return _register_maps.at(_current_fn);
  }

private:
  std::string _output;
  std::string _current_fn = "";
  std::map<std::string, size_t> _stack_loc{};
  size_t _next_stack_loc = 16;
  std::map<std::string, std::map<std::string, std::string>> _register_maps;
  std::vector<std::string> _registers{"x0", "x1", "x2", "x3",
                                      "x4", "x5", "x6", "x7"};
};
