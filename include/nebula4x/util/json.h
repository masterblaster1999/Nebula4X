#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace nebula4x::json {

struct Value;
using Array = std::vector<Value>;
using Object = std::unordered_map<std::string, Value>;

// Minimal JSON value type (null, bool, number, string, array, object).
struct Value : std::variant<std::nullptr_t, bool, double, std::string, Array, Object> {
  using variant::variant;

  bool is_null() const;
  bool is_bool() const;
  bool is_number() const;
  bool is_string() const;
  bool is_array() const;
  bool is_object() const;

  const bool* as_bool() const;
  const double* as_number() const;
  const std::string* as_string() const;
  const Array* as_array() const;
  const Object* as_object() const;

  bool* as_bool();
  double* as_number();
  std::string* as_string();
  Array* as_array();
  Object* as_object();

  // Throws std::runtime_error if not present / wrong type.
  const Value& at(const std::string& key) const;
  const Value& at(std::size_t index) const;

  bool bool_value(bool def = false) const;
  double number_value(double def = 0.0) const;
  std::int64_t int_value(std::int64_t def = 0) const;
  std::string string_value(const std::string& def = "") const;

  // Convenience casts (throw on wrong type)
  const Object& object() const;
  const Array& array() const;

  // Small ergonomic aliases used by some UI code.
  // These mirror the naming used by other JSON libs (e.g. json11).
  const Object& object_items() const { return object(); }
  const Array& array_items() const { return array(); }
};

// Parse a JSON document into a tree.
Value parse(const std::string& text);

// Convert a JSON value to text.
std::string stringify(const Value& v, int indent = 2);

// Helpers to build JSON values.
Value object(Object o);
Value array(Array a);

} // namespace nebula4x::json
