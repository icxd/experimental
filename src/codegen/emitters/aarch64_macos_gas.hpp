#pragma once

#include <codegen/codegen.hpp>
#include <codegen/regalloc.hpp>
#include <common.hpp>

class Aarch64MacosGasEmitter : public Emitter {
public:
  Aarch64MacosGasEmitter(const std::vector<Constant> &constants,
                         const std::vector<Function *> &functions) :
      _constants(constants), _functions(functions) {}

  void emit() override;
  const std::string &output() const override { return _output; }

private:
  void emit_function(Function function) override;
  void emit_instruction(Instruction instr) override;
  std::string emit_operand(Operand operand) override;
  std::string emit_value(Operand operand);
  void store_scratch(const Operand &dst, const std::string &scratch);

  const std::map<std::string, TempAllocation> &current_alloc() const {
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
  std::string _end_label = "";
  std::map<std::string, size_t> _stack_loc{};
  std::map<std::string, size_t> _spill_loc{};
  size_t _next_stack_loc = 16;
  std::map<std::string, std::map<std::string, TempAllocation>> _register_maps;
  std::vector<std::string> _registers{"x0", "x1", "x2", "x3",
                                      "x4", "x5", "x6", "x7"};
};
