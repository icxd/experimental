#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <checker/checker_types.hpp>

struct ModuleSymbols {
  std::string name;
  std::string path;
  std::vector<CheckedProc> procs;
  std::vector<CheckedConst> consts;
};

class ModuleRegistry {
public:
  void register_module(ModuleSymbols symbols) {
    _modules.insert({symbols.name, std::move(symbols)});
  }

  bool has_module(std::string_view name) const {
    return _modules.contains(std::string(name));
  }

  std::optional<CheckedProc> find_proc(std::string_view module,
                                       std::string_view name) const {
    auto it = _modules.find(std::string(module));
    if (it == _modules.end())
      return std::nullopt;
    for (const auto &proc: it->second.procs) {
      if (proc.name == name)
        return proc;
    }
    return std::nullopt;
  }

  std::optional<CheckedConst> find_const(std::string_view module,
                                         std::string_view name) const {
    auto it = _modules.find(std::string(module));
    if (it == _modules.end())
      return std::nullopt;
    for (const auto &constant: it->second.consts) {
      if (constant.name == name)
        return constant;
    }
    return std::nullopt;
  }

  const std::map<std::string, ModuleSymbols> &modules() const {
    return _modules;
  }

private:
  std::map<std::string, ModuleSymbols> _modules;
};
