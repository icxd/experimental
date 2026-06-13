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
  std::vector<CheckedStruct> structs;
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
    auto procs = find_procs(module, name);
    if (procs.empty())
      return std::nullopt;
    return procs.front();
  }

  std::vector<CheckedProc> find_procs(std::string_view module,
                                      std::string_view name) const {
    std::vector<CheckedProc> matches{};
    auto it = _modules.find(std::string(module));
    if (it == _modules.end())
      return matches;
    for (const auto &proc: it->second.procs) {
      if (proc.name == name)
        matches.push_back(proc);
    }
    return matches;
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

  std::optional<CheckedStruct> find_struct(std::string_view module,
                                           std::string_view name) const {
    auto it = _modules.find(std::string(module));
    if (it == _modules.end())
      return std::nullopt;
    for (const auto &strukt: it->second.structs) {
      if (strukt.name == name)
        return strukt;
    }
    return std::nullopt;
  }

  const std::map<std::string, ModuleSymbols> &modules() const {
    return _modules;
  }

private:
  std::map<std::string, ModuleSymbols> _modules;
};
