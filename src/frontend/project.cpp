#include <frontend/project.hpp>

#include <algorithm>
#include <cctype>
#include <queue>
#include <set>
#include <sstream>

#include <lexer/lexer.hpp>
#include <module.hpp>
#include <parser/parser.hpp>

namespace fs = std::filesystem;

std::string project_abs_path(const std::string &path) {
  return fs::weakly_canonical(fs::path(path)).string();
}

std::string project_rel_path(const std::string &path) {
  fs::path canonical = fs::weakly_canonical(fs::path(path));
  fs::path cwd = fs::current_path();
  std::error_code ec;
  fs::path rel = fs::relative(canonical, cwd, ec);
  if (!ec)
    return rel.string();
  return canonical.string();
}

ErrorOr<std::string> project_read_source(const std::string &path) {
  std::fstream f(path);
  if (!f.is_open())
    return std::unexpected(
        Error(std::format("Could not open `{}`", path), 0, 0));
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

ErrorOr<std::vector<std::string>>
project_topo_sort(const std::map<std::string, ParsedModule> &modules) {
  std::map<std::string, int> indegree{};
  std::map<std::string, std::vector<std::string>> dependents{};

  for (const auto &[path, module]: modules) {
    indegree.try_emplace(path, 0);
    for (const auto &import_path: module.import_abs_paths) {
      if (!modules.contains(import_path))
        continue;
      indegree[path]++;
      dependents[import_path].push_back(path);
    }
  }

  std::queue<std::string> ready{};
  for (const auto &[path, degree]: indegree) {
    if (degree == 0)
      ready.push(path);
  }

  std::vector<std::string> order{};
  while (!ready.empty()) {
    std::string path = ready.front();
    ready.pop();
    order.push_back(path);

    for (const auto &dependent: dependents[path]) {
      if (--indegree[dependent] == 0)
        ready.push(dependent);
    }
  }

  if (order.size() != modules.size()) {
    return std::unexpected(
        Error("Circular import detected between modules", 0, 0));
  }

  return order;
}

std::vector<std::string>
project_prioritize_builtins(const std::vector<std::string> &order,
                            const std::map<std::string, ParsedModule> &modules) {
  std::set<std::string> early{};
  std::queue<std::string> pending{};

  for (const auto &[path, module]: modules) {
    if (module.is_runtime || is_prelude_module(module.rel_path)) {
      if (!early.contains(path)) {
        early.insert(path);
        pending.push(path);
      }
    }
  }

  while (!pending.empty()) {
    const ParsedModule &module = modules.at(pending.front());
    pending.pop();
    for (const auto &import_path: module.import_abs_paths) {
      if (!early.contains(import_path)) {
        early.insert(import_path);
        pending.push(import_path);
      }
    }
  }

  std::vector<std::string> prioritized{};
  std::vector<std::string> rest{};
  for (const auto &path: order) {
    if (early.contains(path))
      prioritized.push_back(path);
    else
      rest.push_back(path);
  }
  prioritized.insert(prioritized.end(), rest.begin(), rest.end());
  return prioritized;
}

Project::Project(ProjectOptions opts) : _opts(std::move(opts)) {}

void Project::set_import_paths(std::vector<std::string> paths) {
  _opts.import_paths = std::move(paths);
}

void Project::set_config(CompileConfig config) {
  if (_opts.config.defines != config.defines ||
      _opts.config.target_os != config.target_os ||
      _opts.config.target_arch != config.target_arch)
    _config_generation++;
  _opts.config = std::move(config);
}

void Project::set_document_text(std::string abs_path, std::string text) {
  _document_overrides[project_abs_path(abs_path)] = std::move(text);
}

void Project::remove_document(std::string abs_path) {
  _document_overrides.erase(project_abs_path(abs_path));
}

ErrorOr<ParsedModule>
Project::parse_module(const std::string &path,
                      std::optional<std::string_view> text) {
  ParsedModule module{};
  module.rel_path = project_rel_path(path);
  module.abs_path = project_abs_path(path);
  module.module_name = module_name_from_path(fs::path(module.rel_path));
  module.is_runtime = is_runtime_module(module.rel_path);

  if (text.has_value()) {
    module.source = std::string(text.value());
  } else if (_document_overrides.contains(module.abs_path)) {
    module.source = _document_overrides.at(module.abs_path);
  } else {
    auto source = project_read_source(module.abs_path);
    if (!source.has_value())
      return std::unexpected(source.error());
    module.source = source.value();
  }

  // Tokens store string_views into module.source. Small-string-optimized buffers
  // are invalidated when the module is move-assigned into the project map, so
  // force heap storage before lexing.
  module.source.reserve(64);

  auto with_file = [&](Error error) {
    return error.with_file(module.rel_path);
  };

  Lexer lexer(module.source);
  auto tokens = lexer.lex_tokens();
  if (!tokens.has_value())
    return std::unexpected(with_file(tokens.error()));

  module.arena = std::make_unique<Arena>();
  Parser parser(tokens.value(), *module.arena);
  auto decls = parser.parse_decls();
  if (!decls.has_value())
    return std::unexpected(with_file(decls.error()));
  module.decls = decls.value();

  for (Decl *decl: module.decls) {
    if (decl->type != DECL_IMPORT)
      continue;
    auto *import = std::get<decl::Import *>(decl->data);
    std::vector<fs::path> search_paths{};
    for (const auto &search_path: _opts.import_paths)
      search_paths.push_back(fs::path(search_path));

    auto resolved = resolve_import_path(module.abs_path, import->path.string_value,
                                        search_paths);
    if (!resolved.has_value()) {
      return std::unexpected(with_file(
          Error(std::format("Could not find module `{}`",
                            import->path.string_value),
                decl->start, decl->end)));
    }
    module.import_abs_paths.push_back(project_abs_path(resolved->string()));
  }

  return module;
}

static void scan_imports_from_source(const std::string &importer_abs,
                                     std::string_view source,
                                     const std::vector<fs::path> &search_paths,
                                     ParsedModule &module) {
  size_t pos = 0;
  while (pos < source.size()) {
    pos = source.find("import", pos);
    if (pos == std::string_view::npos)
      break;
    pos += 6;
    while (pos < source.size() &&
           std::isspace(static_cast<unsigned char>(source[pos])))
      pos++;
    if (pos >= source.size() || source[pos] != '"') {
      pos++;
      continue;
    }
    pos++;
    size_t start = pos;
    while (pos < source.size() && source[pos] != '"')
      pos++;
    if (start == pos) {
      pos++;
      continue;
    }
    std::string_view import_path = source.substr(start, pos - start);
    auto resolved =
        resolve_import_path(importer_abs, import_path, search_paths);
    if (resolved.has_value()) {
      std::string abs = project_abs_path(resolved->string());
      if (std::find(module.import_abs_paths.begin(),
                    module.import_abs_paths.end(),
                    abs) == module.import_abs_paths.end())
        module.import_abs_paths.push_back(abs);
    }
    pos++;
  }
}

static ModuleSymbols syntactic_module_symbols(const ParsedModule &module) {
  ModuleSymbols symbols{.name = module.module_name, .path = module.rel_path};
  for (Decl *decl: module.decls) {
    switch (decl->type) {
    case DECL_PROC: {
      auto *proc = std::get<decl::Proc *>(decl->data);
      CheckedProc checked{
          .name = proc->name.id_value,
          .codegen_name = proc->codegen_name,
          .params = {},
          .ret_type = nullptr,
          .scope = nullptr,
          .linkage = proc->linkage,
      };
      if (proc->ret_type.has_value())
        checked.ret_type = proc->ret_type.value();
      for (const Param &param: proc->params)
        checked.params.push_back(
            CheckedParam{.name = param.name.id_value, .type = param.type});
      symbols.procs.push_back(std::move(checked));
      break;
    }
    case DECL_STRUCT: {
      auto *strukt = std::get<decl::Struct *>(decl->data);
      CheckedStruct checked{.name = strukt->name.id_value};
      size_t offset = 0;
      for (const auto &field: strukt->fields) {
        checked.fields.push_back(CheckedStructField{
            .name = field.name.id_value,
            .type = field.type,
            .offset = offset,
        });
        offset += 8;
      }
      checked.size = offset;
      symbols.structs.push_back(std::move(checked));
      break;
    }
    case DECL_CONST: {
      auto *konst = std::get<decl::Const *>(decl->data);
      symbols.consts.push_back(CheckedConst{
          .name = konst->name.id_value,
          .type = konst->type,
          .expr = konst->value,
      });
      break;
    }
    case DECL_ENUM: {
      auto *enum_ = std::get<decl::Enum *>(decl->data);
      CheckedEnum checked{.name = enum_->name.id_value, .underlying_type = nullptr};
      int64_t next_value = 0;
      for (const decl::EnumMember &member: enum_->members) {
        if (member.value.has_value() &&
            member.value.value()->type == EXPR_INT)
          next_value = std::get<expr::Int *>(member.value.value()->data)->value;
        checked.members.push_back(
            CheckedEnumMember{member.name.id_value, next_value});
        next_value++;
      }
      checked.members.push_back(
          CheckedEnumMember{"count",
                            static_cast<int64_t>(enum_->members.size())});
      symbols.enums.push_back(std::move(checked));
      break;
    }
    default:
      break;
    }
  }
  return symbols;
}

ProjectDiagnostic project_diagnostic_from_error(const Error &error,
                                                std::string file_path) {
  if (!error.file_path.empty())
    file_path = error.file_path;
  return ProjectDiagnostic{
      .file_path = std::move(file_path),
      .message = error.message,
      .hint = error.hint,
      .start = error.start,
      .end = error.end,
      .helps = error.helps,
  };
}

ErrorOr<void> Project::discover_from_roots(
    const std::vector<std::string> &roots,
    std::map<std::string, ParsedModule> &discovered) {
  std::queue<std::string> pending{};
  for (const auto &root: roots)
    pending.push(project_abs_path(root));

  while (!pending.empty()) {
    std::string path = pending.front();
    pending.pop();

    if (discovered.contains(path))
      continue;

    std::optional<std::string_view> override_text = std::nullopt;
    if (_document_overrides.contains(path))
      override_text = _document_overrides.at(path);

    auto parsed = parse_module(path, override_text);
    if (!parsed.has_value()) {
      const Error &error = parsed.error();
      _diagnostics.push_back(project_diagnostic_from_error(
          error, error.file_path.empty() ? project_rel_path(path)
                                         : error.file_path));

      ParsedModule stub{};
      stub.abs_path = project_abs_path(path);
      stub.rel_path = project_rel_path(path);
      stub.module_name = module_name_from_path(fs::path(stub.rel_path));
      stub.is_runtime = is_runtime_module(stub.rel_path);
      if (override_text.has_value()) {
        stub.source = std::string(override_text.value());
      } else {
        auto source = project_read_source(stub.abs_path);
        if (source.has_value())
          stub.source = source.value();
      }

      std::vector<fs::path> search_paths{};
      for (const auto &search_path: _opts.import_paths)
        search_paths.push_back(fs::path(search_path));
      scan_imports_from_source(stub.abs_path, stub.source, search_paths, stub);

      discovered.insert({path, std::move(stub)});
      for (const auto &import_path: discovered.at(path).import_abs_paths) {
        if (!discovered.contains(import_path))
          pending.push(import_path);
      }
      continue;
    }

    ParsedModule module = std::move(parsed.value());
    discovered.insert({path, std::move(module)});

    for (const auto &import_path: discovered.at(path).import_abs_paths) {
      if (!discovered.contains(import_path))
        pending.push(import_path);
    }
  }

  return {};
}

bool Project::module_is_fresh(const std::string &path,
                              const ParsedModule &module) {
  auto it = _checked_mtimes.find(path);
  if (it == _checked_mtimes.end())
    return false;
  if (it->second.second != _config_generation)
    return false;
  std::error_code ec;
  auto mtime = fs::last_write_time(module.abs_path, ec);
  if (ec)
    return false;
  return it->second.first == mtime;
}

void Project::restore_registry() {
  _registry = ModuleRegistry{};
  for (const std::string &path: _order) {
    auto it = _checked_symbols.find(path);
    if (it != _checked_symbols.end())
      _registry.register_module(it->second);
  }
}

ErrorOr<void> Project::finalize_modules() {
  std::vector<fs::path> search_paths{};
  for (const auto &search_path: _opts.import_paths)
    search_paths.push_back(fs::path(search_path));

  for (const auto &[path, module]: _modules) {
    for (Decl *decl: module.decls) {
      if (decl->type != DECL_IMPORT)
        continue;
      auto *import = std::get<decl::Import *>(decl->data);
      auto resolved =
          resolve_import_path(module.abs_path, import->path.string_value,
                              search_paths);
      if (!resolved.has_value())
        continue;
      std::string resolved_abs = project_abs_path(resolved->string());
      if (!_modules.contains(resolved_abs)) {
        _diagnostics.push_back(ProjectDiagnostic{
            .file_path = module.rel_path,
            .message = std::format("Could not load imported module `{}`",
                                   import->path.string_value),
            .start = decl->start,
            .end = decl->end,
        });
      }
    }
  }

  auto order = project_topo_sort(_modules);
  if (!order.has_value()) {
    _diagnostics.push_back(ProjectDiagnostic{
        .file_path = "",
        .message = order.error().message,
    });
    return {};
  }

  _order = project_prioritize_builtins(order.value(), _modules);

  for (const std::string &path: _order) {
    ParsedModule &module = _modules.at(path);

    if (module.decls.empty())
      continue;

    if (module_is_fresh(path, module)) {
      _imports_by_path[path] = _checked_imports.at(path);
      continue;
    }

    Checker checker(module.module_name, module.abs_path, &_registry,
                    module.is_runtime, _opts.import_paths, _opts.config,
                    module.arena.get());
    ErrorOr<void> check = checker.check_decls(module.decls);
    if (!check.has_value()) {
      const Error &error = check.error();
      _diagnostics.push_back(
          project_diagnostic_from_error(error, module.rel_path));
      _registry.register_module(syntactic_module_symbols(module));
      continue;
    }

    ModuleSymbols symbols{
        .name = module.module_name,
        .path = module.rel_path,
        .procs = checker.procs(),
        .consts = checker.consts(),
        .structs = checker.structs(),
        .enums = checker.enums(),
    };
    _registry.register_module(symbols);
    _checked_symbols[path] = symbols;
    _checked_imports[path] = checker.imports();
    _imports_by_path[path] = checker.imports();
    module.lambdas = checker.lambdas();
    std::error_code ec;
    _checked_mtimes[path] = {fs::last_write_time(module.abs_path, ec),
                             _config_generation};
  }

  return {};
}

ErrorOr<void> Project::rebuild(const std::vector<std::string> &roots) {
  _modules.clear();
  _order.clear();
  _registry = ModuleRegistry{};
  _diagnostics.clear();
  _imports_by_path.clear();
  _checked_mtimes.clear();
  _checked_symbols.clear();
  _checked_imports.clear();

  try$(discover_from_roots(roots, _modules));
  return finalize_modules();
}

ErrorOr<void> Project::rebuild_test_entry(const std::string &test_path) {
  _diagnostics.clear();

  std::vector<std::string> to_remove{};
  for (const auto &[path, module]: _modules) {
    if (is_test_entry_module(module.rel_path) ||
        module.rel_path.starts_with("tests/"))
      to_remove.push_back(path);
  }
  for (const std::string &path: to_remove) {
    _modules.erase(path);
    _checked_mtimes.erase(path);
    _checked_symbols.erase(path);
    _checked_imports.erase(path);
    _imports_by_path.erase(path);
  }
  restore_registry();

  try$(discover_from_roots({test_path}, _modules));
  return finalize_modules();
}

ParsedModule *Project::find_module(std::string_view abs_path) {
  std::string key = project_abs_path(std::string(abs_path));
  auto it = _modules.find(key);
  if (it == _modules.end())
    return nullptr;
  return &it->second;
}

ParsedModule *Project::find_module_by_rel(std::string_view rel_path) {
  for (auto &[path, module]: _modules) {
    if (module.rel_path == rel_path)
      return &module;
  }
  return nullptr;
}

const std::vector<std::string> &
Project::imports_for(std::string_view abs_path) const {
  static const std::vector<std::string> empty{};
  std::string key = project_abs_path(std::string(abs_path));
  auto it = _imports_by_path.find(key);
  if (it == _imports_by_path.end())
    return empty;
  return it->second;
}
