#include <memory>

#include <arena.hpp>
#include <checker/checker.hpp>
#include <codegen/codegen.hpp>
#include <common.hpp>
#include <host.hpp>
#include <ir/irgen.hpp>
#include <lexer/lexer.hpp>
#include <parser/parser.hpp>
#include <parser/printer.hpp>

namespace fs = std::filesystem;

struct Opts {
  std::vector<std::string> files = {};
  std::string output_file = "a.out";
  bool print_tokens = false;
  bool print_ast = false;
  bool print_ir = false;
};

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
    } else {
      throw std::runtime_error("illegal option");
    }
  }

  for (const std::string &file_path: opts.files) {
    std::fstream f(file_path);
    std::stringstream ss;
    ss << f.rdbuf();
    std::string source = ss.str();

    Lexer lexer(source);
    auto tokens = lexer.lex_tokens();
    if (!tokens.has_value()) {
      Error error = tokens.error();
      print_error(source, file_path, error);
      return EXIT_FAILURE;
    }

    if (opts.print_tokens) {
      for (const auto &token: tokens.value()) {
        std::println("{} @ {}->{} ({})", token.to_string(), token.start,
                     token.end,
                     source.substr(token.start, token.end - token.start));
      }
      continue;
    }

    Arena arena;
    Parser parser(tokens.value(), arena);
    auto decls = parser.parse_decls();
    if (!decls.has_value()) {
      Error error = decls.error();
      print_error(source, file_path, error);
      return EXIT_FAILURE;
    }

    // if (opts.print_ast) {
    //   for (auto *decl: decls.value())
    //     print_decl(std::cout, decl);
    //   continue;
    // }

    Checker checker;
    ErrorOr<void> check = checker.check_decls(decls.value());
    if (!check.has_value()) {
      Error error = check.error();
      print_error(source, file_path, error);
      return EXIT_FAILURE;
    }

    if (opts.print_ast) {
      for (auto *decl: decls.value())
        print_decl(std::cout, decl);
      continue;
    }

    Generator gen(decls.value());
    gen.gen_decls();

    if (opts.print_ir) {
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

    fs::path out_path = dir / file_path;
    out_path = out_path.replace_extension("S");

    if (!fs::exists(out_path.parent_path()))
      fs::create_directory(out_path.parent_path());

    std::ofstream out(out_path);
    out.write(emitter->output().data(),
              static_cast<std::streamsize>(emitter->output().size()));
    out.close();
  }

  std::vector<std::string> object_files{};
  for (const std::string &file_path: opts.files) {
    fs::path dir = fs::current_path() / ".rye";
    if (!fs::exists(dir))
      fs::create_directory(dir);

    fs::path out_path = dir / file_path;
    fs::path s_path = out_path.replace_extension("S");
    fs::path obj_path = out_path.replace_extension("o");

    object_files.push_back(out_path);
    std::string output =
        exec("clang -c -o " + out_path.string() + " " + s_path.string());
  }

  std::string files;
  for (const std::string &path: object_files)
    files += path + " ";

  std::string output = exec("clang -o " + opts.output_file + " " + files);

  return EXIT_SUCCESS;
}
