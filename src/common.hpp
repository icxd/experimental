#pragma once

#include <array>
#include <cstdio>
#include <memory>
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
  struct PipeCloser {
    void operator()(FILE *file) const {
      if (file != nullptr)
        pclose(file);
    }
  };
  std::unique_ptr<FILE, PipeCloser> pipe(popen(cmd.c_str(), "r"));
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
  std::string file_path = "";
  std::vector<ErrorHelp> helps = {};

  Error(std::string msg, size_t s, size_t e) :
      message(std::move(msg)), start(s), end(e) {}

  Error &with_hint(std::string h) {
    hint = std::move(h);
    return *this;
  }

  Error &with_file(std::string path) {
    file_path = std::move(path);
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

static std::string unescape_string(std::string_view raw) {
  std::string out;
  out.reserve(raw.size());
  for (size_t i = 0; i < raw.size(); i++) {
    if (raw[i] == '\\' && i + 1 < raw.size()) {
      char next = raw[++i];
      switch (next) {
      case 'n':  out += '\n'; break;
      case 't':  out += '\t'; break;
      case 'r':  out += '\r'; break;
      case '\\': out += '\\'; break;
      case '"':  out += '"'; break;
      case '0':  out += '\0'; break;
      default:   out += next; break;
      }
    } else {
      out += raw[i];
    }
  }
  return out;
}

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

static std::pair<size_t, size_t> offset_to_line_column(std::string_view source,
                                                       size_t offset) {
  size_t line = 0, column = 0;
  for (size_t i = 0; i < offset && i < source.size(); i++) {
    if (source[i] == '\n') {
      line++;
      column = 0;
    } else {
      column++;
    }
  }
  return {line, column};
}

struct LineSpan {
  size_t start_line;
  size_t start_col;
  size_t end_line;
  size_t end_col;
};

static LineSpan line_span(std::string_view source, size_t start, size_t end) {
  auto lines = split(source, '\n');
  end = std::min(std::max(end, start + 1), source.size());

  auto [start_line, start_col] = offset_to_line_column(source, start);
  auto [end_line, end_col] = offset_to_line_column(source, end);

  if (end > 0 && end <= source.size() && source[end - 1] == '\n' &&
      end_line > start_line) {
    end_line--;
    end_col = lines[end_line].size();
  } else if (end_line > start_line && end_col == 0) {
    end_line--;
    end_col = lines[end_line].size();
  }

  if (end_line == start_line && end_col <= start_col)
    end_col = std::min(start_col + 1, lines[start_line].size());

  return {start_line, start_col, end_line, end_col};
}

static void print_source_snippet(std::string_view source, size_t start,
                                 size_t end, char marker,
                                 const char *marker_color) {
  if (source.empty() || start >= source.size())
    return;

  auto lines = split(source, '\n');
  auto span = line_span(source, start, end);
  size_t line_width = std::to_string(span.end_line + 1).size();

  std::println("\033[34;1m {:>{}} |\033[0m", "", line_width);
  for (size_t line = span.start_line;
       line <= span.end_line && line < lines.size(); line++) {
    std::println("\033[34;1m {:>{}} | \033[0m{}", line + 1, line_width,
                 lines[line]);

    size_t hl_start = (line == span.start_line) ? span.start_col : 0;
    size_t hl_end =
        (line == span.end_line) ? span.end_col : lines[line].size();
    size_t hl_len = std::max<size_t>(1, hl_end > hl_start ? hl_end - hl_start : 1);

    std::print("\033[34;1m {:>{}} | \033[0m", "", line_width);
    std::print("{}", std::string(hl_start, ' '));
    std::println("{}{}{}", marker_color, std::string(hl_len, marker), "\033[0m");
  }
  std::println("\033[34;1m {:>{}} |\033[0m", "", line_width);
}

static void print_error_help(std::string_view source, ErrorHelp help) {
  std::println("\033[1;36m   = help:\033[0m {}", help.message);
  if (!help.replacement.has_value())
    return;

  std::string modified =
      replace_span(help.start, help.end, *help.replacement, std::string(source));
  size_t highlight_end = help.start + help.replacement->size();
  print_source_snippet(modified, help.start, highlight_end, '~', "\033[0;36m");
}

static void print_error(std::string_view source, std::string_view file_path,
                        Error error) {
  if (file_path.empty() && !error.file_path.empty())
    file_path = error.file_path;

  std::println();

  if (source.empty() || error.start >= source.size()) {
    std::println("\033[31;1merror:\033[0m {}", error.message);
    if (!file_path.empty())
      std::println("\033[34;1m   --> {}\033[0m", file_path);
    if (!error.hint.empty())
      std::println("\033[1;36m   = note:\033[0m {}", error.hint);
    for (const auto &help: error.helps)
      print_error_help(source, help);
    return;
  }

  size_t end = std::min(error.end, source.size());
  auto span = line_span(source, error.start, end);
  size_t line_width = std::to_string(span.end_line + 1).size();
  size_t start_line = span.start_line + 1;
  size_t start_col = span.start_col + 1;

  std::println("\033[31;1merror:\033[0m {}", error.message);
  if (span.end_line > span.start_line) {
    std::println("\033[34;1m {:>{}}--> {}:{}:{}-{}:{}\033[0m", "", line_width,
                 file_path, start_line, start_col, span.end_line + 1,
                 span.end_col + 1);
  } else {
    std::println("\033[34;1m {:>{}}--> {}:{}:{}\033[0m", "", line_width,
                 file_path, start_line, start_col);
  }

  print_source_snippet(source, error.start, end, '^', "\033[0;31m");

  if (!error.hint.empty())
    std::println("\033[1;36m   = note:\033[0m {}", error.hint);

  for (const auto &help: error.helps)
    print_error_help(source, help);
}

static std::string json_escape(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (char ch: text) {
    switch (ch) {
    case '\\': out += "\\\\"; break;
    case '"':  out += "\\\""; break;
    case '\n': out += "\\n"; break;
    case '\r': out += "\\r"; break;
    case '\t': out += "\\t"; break;
    default:   out += ch; break;
    }
  }
  return out;
}

static void print_diagnostics_json(std::string_view source,
                                   std::string_view file_path, Error error) {
  if (file_path.empty() && !error.file_path.empty())
    file_path = error.file_path;

  std::string message = error.message;

  if (source.empty() || error.start >= source.size()) {
    if (!error.hint.empty())
      std::println(
          R"({{"diagnostics":[{{"file":"{}","message":"{}","hint":"{}","severity":"error","range":{{"start":{{"line":0,"character":0}},"end":{{"line":0,"character":0}}}}}}]}})",
          json_escape(file_path), json_escape(message), json_escape(error.hint));
    else
      std::println(
          R"({{"diagnostics":[{{"file":"{}","message":"{}","severity":"error","range":{{"start":{{"line":0,"character":0}},"end":{{"line":0,"character":0}}}}}}]}})",
          json_escape(file_path), json_escape(message));
    return;
  }

  auto [start_line, start_col] = offset_to_line_column(source, error.start);
  auto [end_line, end_col] = offset_to_line_column(source, error.end);

  if (!error.hint.empty())
    std::println(
        R"({{"diagnostics":[{{"file":"{}","message":"{}","hint":"{}","severity":"error","range":{{"start":{{"line":{},"character":{}}},"end":{{"line":{},"character":{}}}}}}}]}})",
        json_escape(file_path), json_escape(message), json_escape(error.hint),
        start_line, start_col, end_line, end_col);
  else
    std::println(
        R"({{"diagnostics":[{{"file":"{}","message":"{}","severity":"error","range":{{"start":{{"line":{},"character":{}}},"end":{{"line":{},"character":{}}}}}}}]}})",
        json_escape(file_path), json_escape(message), start_line, start_col,
        end_line, end_col);
}

static void print_diagnostics_ok_json() {
  std::println(R"({{"diagnostics":[]}})");
}

static size_t position_to_offset(std::string_view source, size_t line,
                                 size_t character) {
  size_t cur_line = 0;
  size_t line_start = 0;
  for (size_t i = 0; i <= source.size(); i++) {
    if (cur_line == line) {
      size_t end = i;
      while (end < source.size() && source[end] != '\n')
        end++;
      size_t line_len = end - line_start;
      size_t col = character > line_len ? line_len : character;
      return line_start + col;
    }
    if (i == source.size())
      break;
    if (source[i] == '\n') {
      cur_line++;
      line_start = i + 1;
    }
  }
  return source.size();
}

static bool is_ident_char(char ch) {
  return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_';
}

static std::optional<std::pair<size_t, size_t>>
identifier_span_at(std::string_view source, size_t offset) {
  if (source.empty())
    return std::nullopt;
  if (offset >= source.size())
    offset = source.size() - 1;
  if (!is_ident_char(source[offset]) &&
      offset > 0 &&
      is_ident_char(source[offset - 1]))
    offset--;

  if (!is_ident_char(source[offset]))
    return std::nullopt;

  size_t start = offset;
  while (start > 0 && is_ident_char(source[start - 1]))
    start--;
  size_t end = offset;
  while (end < source.size() && is_ident_char(source[end]))
    end++;
  return std::pair{start, end};
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
