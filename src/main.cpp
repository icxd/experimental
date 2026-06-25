#include <cstdlib>
#include <cstring>
#include <set>

#include <checker/checker.hpp>
#include <codegen/codegen.hpp>
#include <common.hpp>
#include <compile_cache.hpp>
#include <compile_config.hpp>
#include <frontend/project.hpp>
#include <host.hpp>
#include <ir/irgen.hpp>
#include <lexer/lexer.hpp>
#include <lsp/server.hpp>
#include <module.hpp>
#include <parser/printer.hpp>
#include <test_runner.hpp>

namespace fs = std::filesystem;

static void report_error(const Opts &opts, std::string_view source,
                         std::string_view file_path, const Error &error) {
  if (opts.diagnostics_json)
    print_diagnostics_json(source, file_path, error);
  else
    print_error(source, file_path, error);
}

static void report_project_diagnostic(const Opts &opts, const Project &project,
                                      const ProjectDiagnostic &diag) {
  std::string source;
  if (ParsedModule *module = const_cast<Project &>(project).find_module_by_rel(
          diag.file_path);
      module != nullptr)
    source = module->source;
  else if (!diag.file_path.empty()) {
    auto contents = project_read_source(diag.file_path);
    if (contents.has_value())
      source = contents.value();
  }
  Error error(diag.message, diag.start, diag.end);
  error.file_path = diag.file_path;
  error.hint = diag.hint;
  error.helps = diag.helps;
  report_error(opts, source, diag.file_path, error);
}

int main(int argc, char *argv[]) {
  if (argc >= 2 && std::strcmp(argv[1], "lsp") == 0)
    return run_lsp_server();

  char *program = eat_arg();

  Opts opts;
  opts.import_paths.push_back(fs::current_path().string());

  if (argc >= 1 && std::strcmp(argv[0], "test") == 0) {
    eat_arg();
    return run_test_suite(opts, argc, argv);
  }

  opts.files.push_back("runtime/ryert.rye");
  opts.files.push_back("std/string.rye");
  opts.files.push_back("std/compiler.rye");

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
    } else if (strncmp(opt, "--check-only", 12) == 0) {
      opts.check_only = true;
    } else if (strncmp(opt, "--diagnostics-json", 18) == 0) {
      opts.diagnostics_json = true;
    } else if (strncmp(opt, "-I", 2) == 0) {
      opts.import_paths.push_back(argv[i++]);
    } else if (strncmp(opt, "-D", 2) == 0) {
      char *define = argv[i++];
      auto parsed = parse_define_flag(define, opts.config);
      if (!parsed.has_value())
        throw std::runtime_error(parsed.error().message);
    } else if (strncmp(opt, "--target", 8) == 0) {
      char *target = argv[i++];
      auto parsed = parse_target_flag(target, opts.config);
      if (!parsed.has_value())
        throw std::runtime_error(parsed.error().message);
    } else {
      throw std::runtime_error("illegal option");
    }
  }

  ProjectOptions popts{
      .import_paths = opts.import_paths,
      .config = opts.config,
  };
  Project project(popts);

  if (opts.print_tokens) {
    for (const std::string &file_path: opts.files) {
      auto source = project_read_source(file_path);
      if (!source.has_value()) {
        report_error(opts, "", file_path, source.error());
        return EXIT_FAILURE;
      }
      Lexer lexer(source.value());
      auto tokens = lexer.lex_tokens();
      if (!tokens.has_value()) {
        report_error(opts, source.value(), file_path, tokens.error());
        return EXIT_FAILURE;
      }
      for (const auto &token: tokens.value()) {
        std::println("{} @ {}->{} ({})", token.to_string(), token.start,
                     token.end,
                     source.value().substr(token.start, token.end - token.start));
      }
    }
    return EXIT_SUCCESS;
  }

  project.rebuild(opts.files);
  if (!project.diagnostics().empty()) {
    for (const auto &diag: project.diagnostics())
      report_project_diagnostic(opts, project, diag);
    return EXIT_FAILURE;
  }

  const auto &modules = project.modules();
  const auto &order = project.order();
  const ModuleRegistry &registry = project.registry();

  std::set<std::string> print_targets{};
  for (const auto &file_path: opts.files)
    print_targets.insert(project_abs_path(file_path));

  for (const std::string &path: order) {
    ParsedModule &module = const_cast<ParsedModule &>(modules.at(path));

    if (opts.print_ast) {
      if (print_targets.contains(path)) {
        for (auto *decl: module.decls)
          print_decl(std::cout, decl);
      }
      continue;
    }

    if (opts.check_only)
      continue;

    fs::path dir = cache_dir();
    fs::path s_path = dir / module.rel_path;
    s_path.replace_extension("S");

    if (!opts.print_ast && !opts.check_only && !opts.print_ir &&
        fs::exists(module.abs_path) && asm_up_to_date(s_path, module.abs_path))
      continue;

    Generator gen(module.decls, module.module_name,
                  should_mangle_module(module.rel_path), &registry,
                  project.imports_for(path), module.lambdas);
    gen.gen_decls();

    if (opts.print_ir) {
      if (print_targets.contains(path))
        gen.builder().print();
      continue;
    }

    auto codegen_target = target_for_codegen(opts.config);
    if (!codegen_target.has_value()) {
      report_error(opts, "", "", codegen_target.error());
      return EXIT_FAILURE;
    }
    Target target = codegen_target.value();
    std::unique_ptr<Emitter> emitter(Emitter::get_emitter(
        target, gen.builder().constants(), gen.builder().functions(),
        gen.builder().rodata()));
    emitter->emit();

    write_file_if_changed(s_path, emitter->output());
  }

  if (opts.print_ast || opts.print_ir || opts.check_only) {
    if (opts.diagnostics_json)
      print_diagnostics_ok_json();
    return EXIT_SUCCESS;
  }

  std::vector<std::string> object_files{};
  for (const std::string &path: order) {
    const ParsedModule &module = modules.at(path);
    fs::path dir = cache_dir();
    fs::path out_path = dir / module.rel_path;
    fs::path s_path = out_path.replace_extension("S");
    out_path = out_path.replace_extension("o");

    object_files.push_back(out_path.string());
    int status = compile_object(out_path.string(), s_path.string());
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
