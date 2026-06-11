#pragma once

#include <filesystem>
#include <format>
#include <string>
#include <string_view>

#include <parser/ast.hpp>

namespace fs = std::filesystem;

inline std::string module_name_from_path(const fs::path &path) {
  return path.stem().string();
}

inline bool is_runtime_module(const std::string &path) {
  fs::path p(path);
  return p.filename() == "ryert.rye";
}

inline bool should_mangle_module(const std::string &path) {
  return !is_runtime_module(path);
}

inline std::string mangle_symbol(std::string_view module,
                                 std::string_view symbol) {
  return std::format("{}_{}", module, symbol);
}

inline std::string link_name(std::string_view module, std::string_view symbol,
                             bool mangling, Linkage linkage) {
  if (!mangling || linkage == LINK_EXTERN || symbol == "main")
    return std::string(symbol);
  return mangle_symbol(module, symbol);
}

inline fs::path resolve_import_path(const fs::path &importer,
                                    std::string_view import_path) {
  fs::path p(import_path);
  if (p.is_absolute())
    return fs::weakly_canonical(p);
  return fs::weakly_canonical(importer.parent_path() / p);
}
