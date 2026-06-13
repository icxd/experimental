#pragma once

#include <compile_config.hpp>
#include <string>
#include <vector>

struct Opts {
  std::vector<std::string> files = {};
  std::vector<std::string> import_paths = {};
  std::string output_file = "a.out";
  CompileConfig config = {};
  bool print_tokens = false;
  bool print_ast = false;
  bool print_ir = false;
  bool check_only = false;
  bool diagnostics_json = false;
};

int run_test_suite(Opts &opts, int argc, char **argv);
