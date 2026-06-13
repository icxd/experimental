#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <filesystem>

#include <arena.hpp>
#include <checker/checker.hpp>
#include <checker/module_registry.hpp>
#include <common.hpp>
#include <compile_config.hpp>
#include <parser/ast.hpp>

struct ProjectDiagnostic {
  std::string file_path;
  std::string message;
  std::string hint;
  size_t start = 0;
  size_t end = 0;
  std::vector<ErrorHelp> helps = {};
};

ProjectDiagnostic project_diagnostic_from_error(const Error &error,
                                                std::string file_path = "");

struct ParsedModule {
  std::string rel_path;
  std::string abs_path;
  std::string module_name;
  bool is_runtime = false;
  std::string source;
  std::unique_ptr<Arena> arena;
  std::vector<Decl *> decls;
  std::vector<std::string> import_abs_paths;
  std::vector<CheckedLambda> lambdas = {};
};

struct ProjectOptions {
  std::vector<std::string> import_paths = {};
  CompileConfig config = {};
};

class Project {
public:
  explicit Project(ProjectOptions opts = {});

  void set_import_paths(std::vector<std::string> paths);
  void set_config(CompileConfig config);
  void set_document_text(std::string abs_path, std::string text);
  void remove_document(std::string abs_path);

  ErrorOr<void> rebuild(const std::vector<std::string> &roots);
  ErrorOr<void> rebuild_test_entry(const std::string &test_path);

  const std::map<std::string, ParsedModule> &modules() const {
    return _modules;
  }
  const ModuleRegistry &registry() const { return _registry; }
  const std::vector<ProjectDiagnostic> &diagnostics() const {
    return _diagnostics;
  }

  ParsedModule *find_module(std::string_view abs_path);
  ParsedModule *find_module_by_rel(std::string_view rel_path);
  const std::vector<std::string> &order() const { return _order; }
  const std::vector<std::string> &imports_for(std::string_view abs_path) const;

private:
  ErrorOr<ParsedModule>
  parse_module(const std::string &path, std::optional<std::string_view> text);

  ErrorOr<void> discover_from_roots(const std::vector<std::string> &roots,
                                    std::map<std::string, ParsedModule> &into);
  ErrorOr<void> finalize_modules();
  void restore_registry();
  bool module_is_fresh(const std::string &path, const ParsedModule &module);

  ProjectOptions _opts;
  std::map<std::string, ParsedModule> _modules;
  std::vector<std::string> _order;
  ModuleRegistry _registry;
  std::map<std::string, std::string> _document_overrides;
  std::map<std::string, std::vector<std::string>> _imports_by_path;
  std::vector<ProjectDiagnostic> _diagnostics;
  std::map<std::string, std::pair<std::filesystem::file_time_type, uint64_t>>
      _checked_mtimes;
  std::map<std::string, ModuleSymbols> _checked_symbols;
  std::map<std::string, std::vector<std::string>> _checked_imports;
  uint64_t _config_generation = 0;
};

std::string project_abs_path(const std::string &path);
std::string project_rel_path(const std::string &path);
ErrorOr<std::string> project_read_source(const std::string &path);
ErrorOr<std::vector<std::string>>
project_topo_sort(const std::map<std::string, ParsedModule> &modules);
std::vector<std::string>
project_prioritize_builtins(const std::vector<std::string> &order,
                            const std::map<std::string, ParsedModule> &modules);
