#pragma once

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <print>
#include <set>
#include <sstream>
#include <stdexcept>
#include <sys/wait.h>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

#define PANIC(...) panic(__FILE__, __LINE__, std::format(__VA_ARGS__))

__attribute__((noreturn)) static inline void panic(const char *file, int line,
                                                   std::string message) {
  std::println("PANIC in {}:{}! {}", file, line, message);
  abort();
}

#define eat_arg() args_shift(&argc, &argv)
static char *args_shift(int *argc, char ***argv) {
  assert(*argc > 0 && "argc <= 0");
  --(*argc);
  return *(*argv)++;
}

static std::string exec(std::string cmd) {
  std::array<char, 128> buffer;
  std::string result;
  std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"),
                                                pclose);
  if (!pipe)
    throw std::runtime_error("popen() failed!");
  while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe.get()) !=
         nullptr)
    result += buffer.data();
  return result;
}

static int exec_status(const std::string &cmd) {
  int status = std::system(cmd.c_str());
  if (status == -1)
    return -1;
  if (WIFEXITED(status))
    return WEXITSTATUS(status);
  return -1;
}

struct ErrorHelp {
  std::string message;
  size_t start, end;
  std::optional<std::string> replacement = std::nullopt;

  ErrorHelp(std::string msg, size_t s, size_t e,
            std::optional<std::string> r = std::nullopt) :
      message(std::move(msg)), start(s), end(e), replacement(r) {}

  ErrorHelp(std::pair<std::string, std::string> p, size_t s, size_t e) :
      message(std::get<1>(p)), start(s), end(e), replacement(std::get<0>(p)) {}
};

struct Error {
  std::string message;
  size_t start, end;
  std::string hint = "";
  std::vector<ErrorHelp> helps = {};

  Error(std::string msg, size_t s, size_t e) :
      message(std::move(msg)), start(s), end(e) {}

  Error &with_hint(std::string h) {
    hint = std::move(h);
    return *this;
  }

  Error &add_help(std::string msg, size_t s, size_t e) {
    helps.emplace_back(std::move(msg), s, e);
    return *this;
  }

  Error &add_help(ErrorHelp help) {
    helps.push_back(help);
    return *this;
  }

  Error &add_helps(std::initializer_list<ErrorHelp> list) {
    helps.insert(helps.end(), list.begin(), list.end());
    return *this;
  }
};

static std::vector<std::string_view> split(std::string_view str,
                                           char delimiter) {
  std::vector<std::string_view> result;

  size_t start = 0;
  while (start <= str.size()) {
    size_t end = str.find(delimiter, start);
    if (end == std::string_view::npos) {
      end = str.size();
    }
    result.emplace_back(str.substr(start, end - start));
    start = end + 1;
  }

  return result;
}

static std::string replace_span(size_t start, size_t end,
                                std::string replacement,
                                const std::string &source) {
  if (start > end || end > source.size())
    throw std::out_of_range("Invalid span range");

  return source.substr(0, start) + replacement + source.substr(end);
}

static void print_error_help(std::string_view source, ErrorHelp help) {
  size_t line = 1, column = 1;
  for (size_t i = 0; i < help.start; i++) {
    if (source.at(i) == '\n') {
      line++;
      column = 1;
    } else {
      column++;
    }
  }

  std::vector<std::string_view> lines = split(source, '\n');
  std::string code_line = std::string(lines[line - 1]);
  if (help.replacement.has_value())
    code_line = split(replace_span(help.start, help.end, *help.replacement,
                                   std::string(source)),
                      '\n')[line - 1];


  size_t line_width = std::to_string(line).size();

  std::println("\033[1;36mHelp:\033[0m {}", help.message);
  std::println("\033[1;34m {} | \033[0m{}", line, code_line);

  size_t underline_len = std::max<size_t>(1, help.end - help.start);
  std::println("\033[1;34m {} | \033[0;36m{}{}\033[0m",
               std::string(line_width, ' '), std::string(column - 1, ' '),
               std::string(help.replacement.has_value()
                               ? help.replacement->size()
                               : underline_len,
                           '~'));
}

static void print_error(std::string_view source, std::string_view file_path,
                        Error error) {
  size_t line = 1, column = 1;
  for (size_t i = 0; i < error.start; i++) {
    if (source.at(i) == '\n') {
      line++;
      column = 1;
    } else {
      column++;
    }
  }

  std::vector<std::string_view> lines = split(source, '\n');
  size_t line_width = std::to_string(line).size();

  std::println("\033[31;1mError: \033[0m{}", error.message);
  std::println("\033[34;1m {:{}}--> {}:{}:{}\033[0m", ' ',
               std::to_string(line).size(), file_path, line, column);
  std::println("\033[34;1m {} | \033[0m{}", line, lines[line - 1]);

  size_t underline_len = std::max<size_t>(1, error.end - error.start);
  std::println("\033[34;1m {} | \033[0;31m{}{} {}\033[0m",
               std::string(line_width, ' '), std::string(column - 1, ' '),
               std::string(underline_len, '^'), error.hint);

  for (const auto &help: error.helps)
    print_error_help(source, help);
}

template<typename T>
using ErrorOr = std::expected<T, Error>;

#define try$(expr)                                                             \
  ({                                                                           \
    auto __expr = (expr);                                                      \
    if (!__expr.has_value())                                                   \
      return std::unexpected(__expr.error());                                  \
    __expr.value();                                                            \
  })
