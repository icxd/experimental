#pragma once

#include <codegen/codegen.hpp>
#include <codegen/regalloc.hpp>
#include <common.hpp>

class Aarch64LinuxGasEmitter : public Emitter {
public:
  Aarch64LinuxGasEmitter(const std::vector<Constant> &constants,
                         const std::vector<Function *> &functions,
                         const std::vector<RodataEntry> &rodata) :
      _constants(constants), _functions(functions), _rodata(rodata) {}

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
  std::vector<RodataEntry> _rodata;

  std::string _output;
  std::string _current_fn = "";
  const Function *_current_function = nullptr;
  std::string _end_label = "";
  std::map<std::string, size_t> _stack_loc{};
  std::map<std::string, size_t> _var_sizes{};
  std::map<std::string, size_t> _spill_loc{};
  size_t _next_stack_loc = 16;
  std::map<std::string, std::map<std::string, TempAllocation>> _register_maps;
  std::vector<std::string> _registers{"x19", "x20", "x21", "x22"};
};
