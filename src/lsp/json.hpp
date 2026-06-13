#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace lsp::json {

struct Value;

using Array = std::vector<Value>;
using Object = std::map<std::string, Value>;

struct Value {
  using Data = std::variant<std::nullptr_t, bool, double, std::string, Array,
                            Object>;

  Data data = nullptr;

  Value() = default;
  Value(std::nullptr_t) : data(nullptr) {}
  Value(bool v) : data(v) {}
  Value(int v) : data(static_cast<double>(v)) {}
  Value(double v) : data(v) {}
  Value(const char *v) : data(std::string(v)) {}
  Value(std::string v) : data(std::move(v)) {}
  Value(Array v) : data(std::move(v)) {}
  Value(Object v) : data(std::move(v)) {}

  bool is_null() const { return std::holds_alternative<std::nullptr_t>(data); }
  bool is_bool() const { return std::holds_alternative<bool>(data); }
  bool is_number() const { return std::holds_alternative<double>(data); }
  bool is_string() const { return std::holds_alternative<std::string>(data); }
  bool is_array() const { return std::holds_alternative<Array>(data); }
  bool is_object() const { return std::holds_alternative<Object>(data); }

  const std::string &as_string() const {
    return std::get<std::string>(data);
  }
  double as_number() const { return std::get<double>(data); }
  bool as_bool() const { return std::get<bool>(data); }
  const Array &as_array() const { return std::get<Array>(data); }
  const Object &as_object() const { return std::get<Object>(data); }

  Object &as_object_mut() { return std::get<Object>(data); }
  Array &as_array_mut() { return std::get<Array>(data); }

  const Value *find(std::string_view key) const;
  std::string stringify() const;
};

std::optional<Value> parse(std::string_view text);

} // namespace lsp::json
