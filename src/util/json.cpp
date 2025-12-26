#include "nebula4x/util/json.h"

#include <cctype>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace nebula4x::json {
namespace {

struct Parser {
  const std::string& s;
  std::size_t i{0};

  char peek() const { return i < s.size() ? s[i] : '\0'; }
  char get() { return i < s.size() ? s[i++] : '\0'; }

  void skip_ws() {
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
  }

  [[noreturn]] void fail(const std::string& msg) const {
    std::ostringstream ss;
    ss << "JSON parse error at " << i << ": " << msg;
    throw std::runtime_error(ss.str());
  }

  bool consume(char c) {
    skip_ws();
    if (peek() == c) {
      ++i;
      return true;
    }
    return false;
  }

  void expect(char c) {
    skip_ws();
    if (get() != c) fail(std::string("expected '") + c + "'");
  }

  Value parse_value() {
    skip_ws();
    const char c = peek();
    if (c == 'n') return parse_literal("null", nullptr);
    if (c == 't') return parse_literal("true", true);
    if (c == 'f') return parse_literal("false", false);
    if (c == '"') return parse_string();
    if (c == '[') return parse_array();
    if (c == '{') return parse_object();
    if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) return parse_number();
    fail("unexpected character");
  }

  Value parse_literal(const std::string& lit, Value v) {
    for (char c : lit) {
      if (get() != c) fail("invalid literal");
    }
    return v;
  }

  Value parse_number() {
    skip_ws();
    const std::size_t start = i;
    if (peek() == '-') ++i;
    if (!std::isdigit(static_cast<unsigned char>(peek()))) fail("invalid number");
    if (peek() == '0') {
      ++i;
    } else {
      while (std::isdigit(static_cast<unsigned char>(peek()))) ++i;
    }
    if (peek() == '.') {
      ++i;
      if (!std::isdigit(static_cast<unsigned char>(peek()))) fail("invalid number fraction");
      while (std::isdigit(static_cast<unsigned char>(peek()))) ++i;
    }
    if (peek() == 'e' || peek() == 'E') {
      ++i;
      if (peek() == '+' || peek() == '-') ++i;
      if (!std::isdigit(static_cast<unsigned char>(peek()))) fail("invalid exponent");
      while (std::isdigit(static_cast<unsigned char>(peek()))) ++i;
    }
    const std::string num = s.substr(start, i - start);
    try {
      return std::stod(num);
    } catch (...) {
      fail("failed to parse number");
    }
  }

  Value parse_string() {
    expect('"');
    std::string out;
    while (true) {
      if (i >= s.size()) fail("unterminated string");
      char c = get();
      if (c == '"') break;
      if (c == '\\') {
        if (i >= s.size()) fail("bad escape");
        char e = get();
        switch (e) {
          case '"': out.push_back('"'); break;
          case '\\': out.push_back('\\'); break;
          case '/': out.push_back('/'); break;
          case 'b': out.push_back('\b'); break;
          case 'f': out.push_back('\f'); break;
          case 'n': out.push_back('\n'); break;
          case 'r': out.push_back('\r'); break;
          case 't': out.push_back('\t'); break;
          case 'u': {
            // Minimal \uXXXX support: parse but only emit ASCII if possible.
            if (i + 4 > s.size()) fail("bad unicode escape");
            unsigned code = 0;
            for (int k = 0; k < 4; ++k) {
              char h = get();
              code <<= 4;
              if (h >= '0' && h <= '9') code += static_cast<unsigned>(h - '0');
              else if (h >= 'a' && h <= 'f') code += static_cast<unsigned>(h - 'a' + 10);
              else if (h >= 'A' && h <= 'F') code += static_cast<unsigned>(h - 'A' + 10);
              else fail("bad unicode hex");
            }
            if (code <= 0x7F) out.push_back(static_cast<char>(code));
            else out.push_back('?');
            break;
          }
          default: fail("unknown escape");
        }
      } else {
        out.push_back(c);
      }
    }
    return out;
  }

  Value parse_array() {
    expect('[');
    Array arr;
    skip_ws();
    if (consume(']')) return arr;
    while (true) {
      arr.push_back(parse_value());
      skip_ws();
      if (consume(']')) break;
      expect(',');
    }
    return arr;
  }

  Value parse_object() {
    expect('{');
    Object obj;
    skip_ws();
    if (consume('}')) return obj;
    while (true) {
      skip_ws();
      if (peek() != '"') fail("expected string key");
      const std::string key = std::get<std::string>(parse_string());
      expect(':');
      obj[key] = parse_value();
      skip_ws();
      if (consume('}')) break;
      expect(',');
    }
    return obj;
  }
};

std::string escape_string(const std::string& in) {
  std::ostringstream ss;
  ss << '"';
  for (char c : in) {
    switch (c) {
      case '"': ss << "\\\""; break;
      case '\\': ss << "\\\\"; break;
      case '\n': ss << "\\n"; break;
      case '\r': ss << "\\r"; break;
      case '\t': ss << "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          ss << "\\u" << std::hex << std::setw(4) << std::setfill('0') << (int)(unsigned char)c << std::dec;
        } else {
          ss << c;
        }
    }
  }
  ss << '"';
  return ss.str();
}

void stringify_impl(const Value& v, std::ostringstream& out, int indent, int depth) {
  const auto pad = [&](int d) {
    for (int k = 0; k < d * indent; ++k) out << ' ';
  };

  if (v.is_null()) {
    out << "null";
  } else if (v.is_bool()) {
    out << (std::get<bool>(v) ? "true" : "false");
  } else if (v.is_number()) {
    // Keep integers clean when possible.
    const double d = std::get<double>(v);
    if (std::fabs(d - std::round(d)) < 1e-9) {
      out << static_cast<std::int64_t>(std::llround(d));
    } else {
      out << d;
    }
  } else if (v.is_string()) {
    out << escape_string(std::get<std::string>(v));
  } else if (v.is_array()) {
    const auto& a = std::get<Array>(v);
    out << '[';
    if (!a.empty()) {
      if (indent > 0) out << '\n';
      for (std::size_t i = 0; i < a.size(); ++i) {
        if (indent > 0) pad(depth + 1);
        stringify_impl(a[i], out, indent, depth + 1);
        if (i + 1 < a.size()) out << ',';
        if (indent > 0) out << '\n';
      }
      if (indent > 0) pad(depth);
    }
    out << ']';
  } else {
    const auto& o = std::get<Object>(v);
    out << '{';
    if (!o.empty()) {
      if (indent > 0) out << '\n';
      std::size_t n = 0;
      for (const auto& [k, val] : o) {
        if (indent > 0) pad(depth + 1);
        out << escape_string(k) << ':';
        if (indent > 0) out << ' ';
        stringify_impl(val, out, indent, depth + 1);
        if (++n < o.size()) out << ',';
        if (indent > 0) out << '\n';
      }
      if (indent > 0) pad(depth);
    }
    out << '}';
  }
}

} // namespace

bool Value::is_null() const { return std::holds_alternative<std::nullptr_t>(*this); }
bool Value::is_bool() const { return std::holds_alternative<bool>(*this); }
bool Value::is_number() const { return std::holds_alternative<double>(*this); }
bool Value::is_string() const { return std::holds_alternative<std::string>(*this); }
bool Value::is_array() const { return std::holds_alternative<Array>(*this); }
bool Value::is_object() const { return std::holds_alternative<Object>(*this); }

const bool* Value::as_bool() const { return std::get_if<bool>(this); }
const double* Value::as_number() const { return std::get_if<double>(this); }
const std::string* Value::as_string() const { return std::get_if<std::string>(this); }
const Array* Value::as_array() const { return std::get_if<Array>(this); }
const Object* Value::as_object() const { return std::get_if<Object>(this); }

bool* Value::as_bool() { return std::get_if<bool>(this); }
double* Value::as_number() { return std::get_if<double>(this); }
std::string* Value::as_string() { return std::get_if<std::string>(this); }
Array* Value::as_array() { return std::get_if<Array>(this); }
Object* Value::as_object() { return std::get_if<Object>(this); }

const Value& Value::at(const std::string& key) const {
  const auto* o = as_object();
  if (!o) throw std::runtime_error("JSON value is not an object");
  auto it = o->find(key);
  if (it == o->end()) throw std::runtime_error("JSON object missing key: " + key);
  return it->second;
}

const Value& Value::at(std::size_t index) const {
  const auto* a = as_array();
  if (!a) throw std::runtime_error("JSON value is not an array");
  if (index >= a->size()) throw std::runtime_error("JSON array index out of range");
  return (*a)[index];
}

bool Value::bool_value(bool def) const {
  if (auto p = as_bool()) return *p;
  return def;
}

double Value::number_value(double def) const {
  if (auto p = as_number()) return *p;
  return def;
}

std::int64_t Value::int_value(std::int64_t def) const {
  if (auto p = as_number()) return static_cast<std::int64_t>(*p);
  return def;
}

std::string Value::string_value(const std::string& def) const {
  if (auto p = as_string()) return *p;
  return def;
}

const Object& Value::object() const {
  const auto* o = as_object();
  if (!o) throw std::runtime_error("JSON value is not an object");
  return *o;
}

const Array& Value::array() const {
  const auto* a = as_array();
  if (!a) throw std::runtime_error("JSON value is not an array");
  return *a;
}

Value parse(const std::string& text) {
  Parser p{text};
  Value v = p.parse_value();
  p.skip_ws();
  if (p.i != text.size()) throw std::runtime_error("Trailing characters after JSON");
  return v;
}

std::string stringify(const Value& v, int indent) {
  std::ostringstream out;
  stringify_impl(v, out, indent, 0);
  return out.str();
}

Value object(Object o) { return Value(std::move(o)); }
Value array(Array a) { return Value(std::move(a)); }

} // namespace nebula4x::json
