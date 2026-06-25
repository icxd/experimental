#include <test_runner.hpp>

#include <cctype>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <unistd.h>

#include <codegen/codegen.hpp>
#include <compile_cache.hpp>
#include <compile_config.hpp>
#include <frontend/project.hpp>
#include <rye_paths.hpp>
#include <host.hpp>
#include <ir/irgen.hpp>
#include <module.hpp>

namespace fs = std::filesystem;

static std::string trim(std::string_view text) {
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
    text.remove_prefix(1);
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
    text.remove_suffix(1);
  return std::string(text);
}

struct TestCase {
  std::string path;
  int exit_code = 0;
  std::optional<std::string> target;
  bool compile_only = false;
  bool expect_error = false;
  std::vector<std::string> import_paths;
  std::vector<std::string> defines;
};

static std::string host_triple() {
#if defined(__APPLE__)
  const char *os = "macos";
#elif defined(__linux__)
  const char *os = "linux";
#else
  const char *os = "unknown";
#endif
#if defined(__aarch64__) || defined(__arm64__)
  const char *arch = "aarch64";
#else
  const char *arch = "x86_64";
#endif
  return std::format("{}-{}", os, arch);
}

static std::optional<TestCase> parse_test_case(const std::string &path) {
  std::ifstream in(path);
  if (!in.is_open())
    return std::nullopt;

  std::string first;
  if (!std::getline(in, first) || !first.starts_with("/// "))
    return std::nullopt;

  TestCase test{.path = path};
  try {
    test.exit_code = std::stoi(first.substr(4));
  } catch (...) {
    return std::nullopt;
  }

  std::string line;
  while (std::getline(in, line)) {
    if (!line.starts_with("/// "))
      break;
    std::string_view meta = line;
    meta.remove_prefix(4);
    if (meta.starts_with("import-path:")) {
      test.import_paths.emplace_back(trim(meta.substr(12)));
    } else if (meta.starts_with("define:")) {
      test.defines.emplace_back(trim(meta.substr(7)));
    } else if (meta.starts_with("target:")) {
      test.target = trim(meta.substr(7));
    } else if (meta == "compile-only" || meta == "check-only") {
      test.compile_only = true;
    } else if (meta == "expect-error") {
      test.expect_error = true;
    }
  }

  return test;
}

static int compile_project_modules(const Opts &opts, Project &project) {
  const auto &modules = project.modules();
  const auto &order = project.order();
  const ModuleRegistry &registry = project.registry();
  fs::path dir = cache_dir();

  for (const std::string &path: order) {
    ParsedModule &module = const_cast<ParsedModule &>(modules.at(path));
    fs::path s_path = dir / module.rel_path;
    s_path.replace_extension("S");

    if (asm_up_to_date(s_path, module.abs_path))
      continue;

    Generator gen(module.decls, module.module_name,
                  should_mangle_module(module.rel_path), &registry,
                  project.imports_for(path), module.lambdas);
    gen.gen_decls();

    auto codegen_target = target_for_codegen(opts.config);
    if (!codegen_target.has_value())
      return EXIT_FAILURE;
    Target target = codegen_target.value();
    std::unique_ptr<Emitter> emitter(Emitter::get_emitter(
        target, gen.builder().constants(), gen.builder().functions(),
        gen.builder().rodata()));
    emitter->emit();
    write_file_if_changed(s_path, emitter->output());
  }

  for (const std::string &path: order) {
    fs::path out_path = dir / modules.at(path).rel_path;
    fs::path s_path = out_path;
    s_path.replace_extension("S");
    out_path.replace_extension("o");
    if (compile_object(out_path.string(), s_path.string()) != 0)
      return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}

static int link_objects(const std::vector<std::string> &object_files,
                        const std::string &output) {
  std::string files;
  for (const std::string &path: object_files)
    files += path + " ";
  if (exec_status("clang -o " + output + " " + files) != 0)
    return EXIT_FAILURE;
  return EXIT_SUCCESS;
}

static void emit_test_event(std::string_view status, const TestCase &test,
                            std::string_view detail = "") {
  if (!detail.empty())
    std::println("@test {} {} {}", status, test.path, detail);
  else
    std::println("@test {} {}", status, test.path);
}

static int run_one_test(Opts &base_opts, Project &project, const TestCase &test) {
  Opts opts = base_opts;
  opts.config = base_opts.config;
  opts.import_paths = base_opts.import_paths;
  for (const auto &import_path: test.import_paths)
    opts.import_paths.push_back(import_path);
  for (const auto &define: test.defines) {
    auto parsed = parse_define_flag(define, opts.config);
    if (!parsed.has_value())
      return EXIT_FAILURE;
  }
  if (test.target.has_value()) {
    auto parsed = parse_target_flag(*test.target, opts.config);
    if (!parsed.has_value())
      return EXIT_FAILURE;
  }

  project.set_import_paths(opts.import_paths);
  project.set_config(opts.config);

  auto rebuilt = project.rebuild_test_entry(test.path);
  if (!rebuilt.has_value())
    return EXIT_FAILURE;

  if (!project.diagnostics().empty()) {
    if (test.compile_only && test.expect_error) {
      emit_test_event("PASS", test);
      return 0;
    }
    emit_test_event("FAIL", test, "compile");
    return 1;
  }

  if (test.compile_only) {
    if (test.expect_error) {
      emit_test_event("FAIL", test, "expected-error");
      return 1;
    }
    emit_test_event("PASS", test);
    return 0;
  }

  if (compile_project_modules(opts, project) != EXIT_SUCCESS) {
    emit_test_event("FAIL", test, "codegen");
    return 1;
  }

  std::vector<std::string> object_files{};
  fs::path dir = cache_dir();
  for (const std::string &path: project.order()) {
    fs::path out_path = dir / project.modules().at(path).rel_path;
    object_files.push_back(out_path.replace_extension("o").string());
  }

  fs::path output =
      fs::path("tests") / fs::path(test.path).stem();
  if (link_objects(object_files, output.string()) != EXIT_SUCCESS) {
    emit_test_event("FAIL", test, "link");
    return 1;
  }

  int status = exec_status(output.string());
  if (status != test.exit_code) {
    emit_test_event("FAIL", test, std::format("exit:{}:{}", status, test.exit_code));
    return 1;
  }

  emit_test_event("PASS", test);
  return 0;
}

int run_test_suite(Opts &opts, int argc, char **argv) {
  std::vector<TestCase> tests{};
  for (int i = 0; i < argc; i++) {
    auto parsed = parse_test_case(argv[i]);
    if (!parsed.has_value())
      continue;
    tests.push_back(std::move(*parsed));
  }

  ProjectOptions popts{
      .import_paths = opts.import_paths,
      .config = opts.config,
  };
  Project project(popts);

  std::vector<std::string> roots = rye_builtin_modules();
  if (!project.rebuild(roots).has_value())
    return EXIT_FAILURE;
  if (!project.diagnostics().empty())
    return EXIT_FAILURE;

  if (compile_project_modules(opts, project) != EXIT_SUCCESS)
    return EXIT_FAILURE;

  int failures = 0;
  int skipped = 0;
  for (const TestCase &test: tests) {
    if (test.target.has_value() && test.target.value() != host_triple()) {
      emit_test_event("SKIP", test, *test.target);
      skipped++;
      continue;
    }
    failures += run_one_test(opts, project, test);
  }

  int passed = static_cast<int>(tests.size()) - failures - skipped;
  std::println("@summary {} {} {}", passed, skipped, failures);
  return failures > 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
