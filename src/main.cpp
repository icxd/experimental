#include <memory>
#include <queue>
#include <set>

#include <arena.hpp>
#include <checker/checker.hpp>
#include <checker/module_registry.hpp>
#include <codegen/codegen.hpp>
#include <common.hpp>
#include <host.hpp>
#include <ir/irgen.hpp>
#include <lexer/lexer.hpp>
#include <module.hpp>
#include <parser/parser.hpp>
#include <parser/printer.hpp>

namespace fs = std::filesystem;

struct Opts {
  std::vector<std::string> files = {};
  std::vector<std::string> import_paths = {};
  std::string output_file = "a.out";
  bool print_tokens = false;
  bool print_ast = false;
  bool print_ir = false;
};

struct ParsedModule {
  std::string rel_path;
  std::string abs_path;
  std::string module_name;
  bool is_runtime = false;
  std::string source;
  std::unique_ptr<Arena> arena;
  std::vector<Decl *> decls;
  std::vector<std::string> import_abs_paths;
};

static std::string abs_path_of(const std::string &path) {
  return fs::weakly_canonical(fs::path(path)).string();
}

static std::string rel_path_of(const std::string &path) {
  fs::path canonical = fs::weakly_canonical(fs::path(path));
  fs::path cwd = fs::current_path();
  std::error_code ec;
  fs::path rel = fs::relative(canonical, cwd, ec);
  if (!ec)
    return rel.string();
  return canonical.string();
}

static ErrorOr<std::string> read_source(const std::string &path) {
  std::fstream f(path);
  if (!f.is_open())
    return std::unexpected(
        Error(std::format("Could not open `{}`", path), 0, 0));
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

static ErrorOr<ParsedModule>
parse_module(const std::string &path,
             const std::vector<std::string> &import_search_paths) {
  ParsedModule module{};
  module.rel_path = rel_path_of(path);
  module.abs_path = abs_path_of(path);
  module.module_name = module_name_from_path(fs::path(module.rel_path));
  module.is_runtime = is_runtime_module(module.rel_path);

  auto source = read_source(module.rel_path);
  if (!source.has_value())
    return std::unexpected(source.error());
  module.source = source.value();

  Lexer lexer(module.source);
  auto tokens = lexer.lex_tokens();
  if (!tokens.has_value())
    return std::unexpected(tokens.error());

  module.arena = std::make_unique<Arena>();
  Parser parser(tokens.value(), *module.arena);
  auto decls = parser.parse_decls();
  if (!decls.has_value())
    return std::unexpected(decls.error());
  module.decls = decls.value();

  for (Decl *decl: module.decls) {
    if (decl->type != DECL_IMPORT)
      continue;
    auto *import = std::get<decl::Import *>(decl->data);
    std::vector<fs::path> search_paths{};
    for (const auto &search_path: import_search_paths)
      search_paths.push_back(fs::path(search_path));

    auto resolved = resolve_import_path(module.abs_path, import->path.string_value,
                                        search_paths);
    if (!resolved.has_value()) {
      return std::unexpected(
          Error(std::format("Could not find module `{}`",
                            import->path.string_value),
                decl->start, decl->end));
    }
    module.import_abs_paths.push_back(abs_path_of(resolved->string()));
  }

  return module;
}

static ErrorOr<std::map<std::string, ParsedModule>>
discover_modules(const std::vector<std::string> &roots,
                 const std::vector<std::string> &import_search_paths) {
  std::map<std::string, ParsedModule> modules{};
  std::queue<std::string> pending{};

  for (const auto &root: roots)
    pending.push(abs_path_of(root));

  while (!pending.empty()) {
    std::string path = pending.front();
    pending.pop();

    if (modules.contains(path))
      continue;

    auto parsed = parse_module(path, import_search_paths);
    if (!parsed.has_value())
      return std::unexpected(parsed.error());
    ParsedModule module = std::move(parsed.value());
    modules.insert({path, std::move(module)});

    for (const auto &import_path: modules.at(path).import_abs_paths) {
      if (!modules.contains(import_path))
        pending.push(import_path);
    }
  }

  return modules;
}

static ErrorOr<std::vector<std::string>>
topo_sort_modules(const std::map<std::string, ParsedModule> &modules) {
  std::map<std::string, int> indegree{};
  std::map<std::string, std::vector<std::string>> dependents{};

  for (const auto &[path, module]: modules) {
    indegree.try_emplace(path, 0);
    for (const auto &import_path: module.import_abs_paths) {
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

int main(int argc, char *argv[]) {
  char *program = eat_arg();

  Opts opts;
  opts.files.push_back("runtime/ryert.rye");

  for (int i = 0; i < argc;) {
    char *opt = argv[i++];
    if (opt[0] != '-') {
      opts.files.push_back(opt);
    } else if (strncmp(opt, "-O", 2) == 0 || strncmp(opt, "--output", 8) == 0) {
      char *output_file = argv[i++];
      opts.output_file = output_file;
    } else if (strncmp(opt, "--print-tokens", 14) == 0) {
      opts.print_tokens = true;
    } else if (strncmp(opt, "--print-ast", 11) == 0) {
      opts.print_ast = true;
    } else if (strncmp(opt, "--print-ir", 10) == 0) {
      opts.print_ir = true;
    } else if (strncmp(opt, "-I", 2) == 0) {
      opts.import_paths.push_back(argv[i++]);
    } else {
      throw std::runtime_error("illegal option");
    }
  }

  auto discovered = discover_modules(opts.files, opts.import_paths);
  if (!discovered.has_value()) {
    print_error("", "", discovered.error());
    return EXIT_FAILURE;
  }
  auto &modules = discovered.value();

  if (opts.print_tokens) {
    for (const std::string &file_path: opts.files) {
      const auto &module = modules.at(abs_path_of(file_path));
      Lexer lexer(module.source);
      auto tokens = lexer.lex_tokens();
      if (!tokens.has_value()) {
        print_error(module.source, module.rel_path, tokens.error());
        return EXIT_FAILURE;
      }
      for (const auto &token: tokens.value()) {
        std::println("{} @ {}->{} ({})", token.to_string(), token.start,
                     token.end,
                     module.source.substr(token.start, token.end - token.start));
      }
    }
    return EXIT_SUCCESS;
  }

  auto order = topo_sort_modules(modules);
  if (!order.has_value()) {
    print_error("", "", order.error());
    return EXIT_FAILURE;
  }

  ModuleRegistry registry{};
  std::set<std::string> print_targets{};
  for (const auto &file_path: opts.files)
    print_targets.insert(abs_path_of(file_path));

  for (const std::string &path: order.value()) {
    ParsedModule &module = modules.at(path);

    Checker checker(module.module_name, module.abs_path, &registry,
                    module.is_runtime, opts.import_paths);
    ErrorOr<void> check = checker.check_decls(module.decls);
    if (!check.has_value()) {
      print_error(module.source, module.rel_path, check.error());
      return EXIT_FAILURE;
    }

    registry.register_module(ModuleSymbols{
        .name = module.module_name,
        .path = module.rel_path,
        .procs = checker.procs(),
        .consts = checker.consts(),
    });

    if (opts.print_ast) {
      if (print_targets.contains(path)) {
        for (auto *decl: module.decls)
          print_decl(std::cout, decl);
      }
      continue;
    }

    Generator gen(module.decls, module.module_name,
                  should_mangle_module(module.rel_path), &registry,
                  checker.imports());
    gen.gen_decls();

    if (opts.print_ir) {
      if (print_targets.contains(path))
        gen.builder().print();
      continue;
    }

    Target target = get_host_target();
    std::unique_ptr<Emitter> emitter(Emitter::get_emitter(
        target, gen.builder().constants(), gen.builder().functions()));
    emitter->emit();

    fs::path dir = fs::current_path() / ".rye";
    if (!fs::exists(dir))
      fs::create_directory(dir);

    fs::path out_path = dir / module.rel_path;
    out_path = out_path.replace_extension("S");

    if (!fs::exists(out_path.parent_path()))
      fs::create_directory(out_path.parent_path());

    std::ofstream out(out_path);
    out.write(emitter->output().data(),
              static_cast<std::streamsize>(emitter->output().size()));
    out.close();
  }

  if (opts.print_ast || opts.print_ir)
    return EXIT_SUCCESS;

  std::vector<std::string> object_files{};
  for (const std::string &path: order.value()) {
    const ParsedModule &module = modules.at(path);
    fs::path dir = fs::current_path() / ".rye";
    fs::path out_path = dir / module.rel_path;
    fs::path s_path = out_path.replace_extension("S");
    out_path = out_path.replace_extension("o");

    object_files.push_back(out_path.string());
    int status = exec_status("clang -c -o " + out_path.string() + " " +
                             s_path.string());
    if (status != 0)
      return EXIT_FAILURE;
  }

  std::string files;
  for (const std::string &path: object_files)
    files += path + " ";

  if (exec_status("clang -o " + opts.output_file + " " + files) != 0)
    return EXIT_FAILURE;

  return EXIT_SUCCESS;
}
