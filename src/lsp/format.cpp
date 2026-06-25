#include <lsp/format.hpp>

#include <cctype>

std::string format_rye_source(std::string_view source) {
  std::string out;
  int indent = 0;
  bool at_line_start = true;
  bool pending_space = false;

  auto append_indent = [&]() {
    for (int i = 0; i < indent; i++)
      out += "  ";
    at_line_start = false;
  };

  for (size_t i = 0; i < source.size(); i++) {
    char ch = source[i];

    if (ch == '\r')
      continue;

    if (ch == '\n') {
      out += '\n';
      at_line_start = true;
      pending_space = false;
      continue;
    }

    if (std::isspace(static_cast<unsigned char>(ch))) {
      if (!at_line_start)
        pending_space = true;
      continue;
    }

    if (at_line_start)
      append_indent();

    if (pending_space && !at_line_start) {
      out += ' ';
      pending_space = false;
    }

    if (ch == '{') {
      out += '{';
      indent++;
      continue;
    }

    if (ch == '}') {
      indent = std::max(0, indent - 1);
      out += '}';
      continue;
    }

    if (ch == ';') {
      out += ';';
      continue;
    }

    out += ch;
  }

  if (!out.empty() && out.back() != '\n')
    out += '\n';

  return out;
}
