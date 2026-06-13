#include <lsp/json.hpp>

#include <cctype>
#include <format>
#include <sstream>

namespace lsp::json {

namespace {

void skip_ws(std::string_view text, size_t &pos) {
  while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos])))
    pos++;
}

std::optional<Value> parse_value(std::string_view text, size_t &pos);

std::optional<std::string> parse_string(std::string_view text, size_t &pos) {
  if (pos >= text.size() || text[pos] != '"')
    return std::nullopt;
  pos++;
  std::string out;
  while (pos < text.size()) {
    char ch = text[pos++];
    if (ch == '"')
      return out;
    if (ch == '\\' && pos < text.size()) {
      char esc = text[pos++];
      switch (esc) {
      case '"':  out += '"'; break;
      case '\\': out += '\\'; break;
      case '/':  out += '/'; break;
      case 'n':  out += '\n'; break;
      case 'r':  out += '\r'; break;
      case 't':  out += '\t'; break;
      default:   out += esc; break;
      }
    } else {
      out += ch;
    }
  }
  return std::nullopt;
}

std::optional<Value> parse_number(std::string_view text, size_t &pos) {
  size_t start = pos;
  if (pos < text.size() && text[pos] == '-')
    pos++;
  while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos])))
    pos++;
  if (pos < text.size() && text[pos] == '.') {
    pos++;
    while (pos < text.size() &&
           std::isdigit(static_cast<unsigned char>(text[pos])))
      pos++;
  }
  if (start == pos)
    return std::nullopt;
  double value = std::stod(std::string(text.substr(start, pos - start)));
  return Value(value);
}

std::optional<Value> parse_array(std::string_view text, size_t &pos) {
  if (text[pos++] != '[')
    return std::nullopt;
  skip_ws(text, pos);
  Array items{};
  if (pos < text.size() && text[pos] == ']') {
    pos++;
    return Value(std::move(items));
  }
  while (pos < text.size()) {
    auto item = parse_value(text, pos);
    if (!item.has_value())
      return std::nullopt;
    items.push_back(std::move(item.value()));
    skip_ws(text, pos);
    if (pos < text.size() && text[pos] == ',') {
      pos++;
      skip_ws(text, pos);
      continue;
    }
    if (pos < text.size() && text[pos] == ']') {
      pos++;
      return Value(std::move(items));
    }
    return std::nullopt;
  }
  return std::nullopt;
}

std::optional<Value> parse_object(std::string_view text, size_t &pos) {
  if (text[pos++] != '{')
    return std::nullopt;
  skip_ws(text, pos);
  Object obj{};
  if (pos < text.size() && text[pos] == '}') {
    pos++;
    return Value(std::move(obj));
  }
  while (pos < text.size()) {
    auto key = parse_string(text, pos);
    if (!key.has_value())
      return std::nullopt;
    skip_ws(text, pos);
    if (pos >= text.size() || text[pos++] != ':')
      return std::nullopt;
    skip_ws(text, pos);
    auto value = parse_value(text, pos);
    if (!value.has_value())
      return std::nullopt;
    obj.emplace(std::move(key.value()), std::move(value.value()));
    skip_ws(text, pos);
    if (pos < text.size() && text[pos] == ',') {
      pos++;
      skip_ws(text, pos);
      continue;
    }
    if (pos < text.size() && text[pos] == '}') {
      pos++;
      return Value(std::move(obj));
    }
    return std::nullopt;
  }
  return std::nullopt;
}

std::optional<Value> parse_value(std::string_view text, size_t &pos) {
  skip_ws(text, pos);
  if (pos >= text.size())
    return std::nullopt;

  if (text[pos] == '{')
    return parse_object(text, pos);
  if (text[pos] == '[')
    return parse_array(text, pos);
  if (text[pos] == '"') {
    auto str = parse_string(text, pos);
    if (!str.has_value())
      return std::nullopt;
    return Value(std::move(str.value()));
  }
  if (text.substr(pos, 4) == "true") {
    pos += 4;
    return Value(true);
  }
  if (text.substr(pos, 5) == "false") {
    pos += 5;
    return Value(false);
  }
  if (text.substr(pos, 4) == "null") {
    pos += 4;
    return Value(nullptr);
  }
  return parse_number(text, pos);
}

std::string escape_string(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (unsigned char ch: text) {
    switch (ch) {
    case '\\': out += "\\\\"; break;
    case '"':  out += "\\\""; break;
    case '\n': out += "\\n"; break;
    case '\r': out += "\\r"; break;
    case '\t': out += "\\t"; break;
    default:
      if (ch < 0x20)
        out += std::format("\\u{:04x}", ch);
      else
        out += static_cast<char>(ch);
      break;
    }
  }
  return out;
}

std::string stringify_value(const Value &value);

std::string stringify_array(const Array &array) {
  std::string out = "[";
  for (size_t i = 0; i < array.size(); i++) {
    if (i > 0)
      out += ",";
    out += stringify_value(array[i]);
  }
  out += "]";
  return out;
}

std::string stringify_object(const Object &object) {
  std::string out = "{";
  bool first = true;
  for (const auto &[key, value]: object) {
    if (!first)
      out += ",";
    first = false;
    out += std::format("\"{}\":{}", escape_string(key), stringify_value(value));
  }
  out += "}";
  return out;
}

std::string stringify_value(const Value &value) {
  if (value.is_null())
    return "null";
  if (value.is_bool())
    return value.as_bool() ? "true" : "false";
  if (value.is_number()) {
    double n = value.as_number();
    if (n == static_cast<int64_t>(n))
      return std::to_string(static_cast<int64_t>(n));
    return std::to_string(n);
  }
  if (value.is_string())
    return std::format("\"{}\"", escape_string(value.as_string()));
  if (value.is_array())
    return stringify_array(value.as_array());
  return stringify_object(value.as_object());
}

} // namespace

const Value *Value::find(std::string_view key) const {
  if (!is_object())
    return nullptr;
  const auto &obj = as_object();
  auto it = obj.find(std::string(key));
  if (it == obj.end())
    return nullptr;
  return &it->second;
}

std::string Value::stringify() const { return stringify_value(*this); }

std::optional<Value> parse(std::string_view text) {
  size_t pos = 0;
  auto value = parse_value(text, pos);
  if (!value.has_value())
    return std::nullopt;
  skip_ws(text, pos);
  if (pos != text.size())
    return std::nullopt;
  return value;
}

} // namespace lsp::json
