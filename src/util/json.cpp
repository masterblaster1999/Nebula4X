#include "nebula4x/util/json.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <vector>

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
    // We track `i` as a raw byte offset; for error messages, it's more helpful to also show
    // line/column and a small context snippet (handy for modding content JSON).
    const std::size_t pos = std::min(i, s.size());

    // Compute 1-based line/column, plus the [line_start, line_end) bounds.
    //
    // Notes:
    //  - Treat CRLF as a single newline for accurate line/col reporting on Windows-edited files.
    //  - Ignore a UTF-8 BOM (0xEF 0xBB 0xBF) if present at the very start of the document.
    std::size_t line_start = 0;
    std::size_t scan = 0;

    const bool has_bom =
        (s.size() >= 3 &&
         static_cast<unsigned char>(s[0]) == 0xEF &&
         static_cast<unsigned char>(s[1]) == 0xBB &&
         static_cast<unsigned char>(s[2]) == 0xBF);

    if (has_bom) {
      line_start = 3;
      scan = 3;
    }

    int line = 1;
    int col = 1;

    // If the failure is somehow inside the BOM bytes, clamp.
    if (scan > pos) {
      scan = pos;
      line_start = pos;
    }

    while (scan < pos && scan < s.size()) {
      const char ch = s[scan];
      if (ch == '\n') {
        ++line;
        col = 1;
        line_start = scan + 1;
        ++scan;
        continue;
      }
      if (ch == '\r') {
        ++line;
        col = 1;
        // CRLF counts as a single newline.
        if (scan + 1 < s.size() && s[scan + 1] == '\n') {
          scan += 2;
          line_start = scan;
        } else {
          ++scan;
          line_start = scan;
        }
        continue;
      }
      ++col;
      ++scan;
    }

    std::size_t line_end = line_start;
    while (line_end < s.size() && s[line_end] != '\n' && s[line_end] != '\r') ++line_end;

    // Build a context snippet. If the line is very long, trim and add ellipses.
    const std::size_t line_len = line_end - line_start;
    const std::size_t in_line = (pos >= line_start) ? (pos - line_start) : 0;

    std::size_t snippet_start = line_start;
    std::size_t snippet_end = line_end;
    bool prefix_ellipsis = false;
    bool suffix_ellipsis = false;

    constexpr std::size_t kContextBefore = 80;
    constexpr std::size_t kContextAfter = 80;
    constexpr std::size_t kMaxContextLine = kContextBefore + kContextAfter;

    if (line_len > kMaxContextLine) {
      snippet_start = (in_line > kContextBefore) ? (pos - kContextBefore) : line_start;
      snippet_end = std::min(line_end, pos + kContextAfter);
      if (snippet_start > line_start) prefix_ellipsis = true;
      if (snippet_end < line_end) suffix_ellipsis = true;
    }

    std::string snippet;
    if (prefix_ellipsis) snippet += "...";
    if (snippet_end > snippet_start) snippet += s.substr(snippet_start, snippet_end - snippet_start);
    if (suffix_ellipsis) snippet += "...";

    std::size_t caret_pos = (prefix_ellipsis ? 3 : 0) + ((pos >= snippet_start) ? (pos - snippet_start) : 0);
    if (caret_pos > snippet.size()) caret_pos = snippet.size();

    std::ostringstream ss;
    ss << "JSON parse error at " << i << " (line " << line << ", col " << col << "): " << msg;
    if (!snippet.empty()) {
      ss << "\n" << snippet << "\n";
      ss << std::string(caret_pos, ' ') << "^";
    }
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

  unsigned parse_hex4() {
    if (i + 4 > s.size()) fail("bad unicode escape");
    unsigned code = 0;
    for (int k = 0; k < 4; ++k) {
      char h = get();
      code <<= 4;
      if (h >= '0' && h <= '9')
        code += static_cast<unsigned>(h - '0');
      else if (h >= 'a' && h <= 'f')
        code += static_cast<unsigned>(h - 'a' + 10);
      else if (h >= 'A' && h <= 'F')
        code += static_cast<unsigned>(h - 'A' + 10);
      else
        fail("bad unicode hex");
    }
    return code;
  }

  void append_utf8(std::uint32_t codepoint, std::string& out) {
    if (codepoint <= 0x7F) {
      out.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7FF) {
      out.push_back(static_cast<char>(0xC0 | ((codepoint >> 6) & 0x1F)));
      out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else if (codepoint <= 0xFFFF) {
      out.push_back(static_cast<char>(0xE0 | ((codepoint >> 12) & 0x0F)));
      out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else if (codepoint <= 0x10FFFF) {
      out.push_back(static_cast<char>(0xF0 | ((codepoint >> 18) & 0x07)));
      out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else {
      fail("unicode codepoint out of range");
    }
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
            // Decode a JSON \uXXXX escape into UTF-8.
            // JSON escapes are UTF-16 code units; handle surrogate pairs.
            const unsigned hi = parse_hex4();

            // High surrogate: must be followed by another \uXXXX with a low surrogate.
            if (hi >= 0xD800 && hi <= 0xDBFF) {
              if (i + 2 > s.size()) fail("bad unicode surrogate pair");
              if (get() != '\\' || get() != 'u') fail("expected low surrogate");
              const unsigned lo = parse_hex4();
              if (lo < 0xDC00 || lo > 0xDFFF) fail("invalid low surrogate");

              const std::uint32_t codepoint = 0x10000u + (((hi - 0xD800u) << 10u) | (lo - 0xDC00u));
              append_utf8(codepoint, out);
            } else if (hi >= 0xDC00 && hi <= 0xDFFF) {
              // Low surrogate without a preceding high surrogate.
              fail("unexpected low surrogate");
            } else {
              append_utf8(static_cast<std::uint32_t>(hi), out);
            }
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

      // Deterministic output: sort keys (Object is an unordered_map).
      std::vector<std::string> keys;
      keys.reserve(o.size());
      for (const auto& [k, _] : o) keys.push_back(k);
      std::sort(keys.begin(), keys.end());

      std::size_t n = 0;
      for (const auto& k : keys) {
        const auto it = o.find(k);
        if (it == o.end()) continue;
        const auto& val = it->second;

        if (indent > 0) pad(depth + 1);
        out << escape_string(k) << ':';
        if (indent > 0) out << ' ';
        stringify_impl(val, out, indent, depth + 1);
        if (++n < keys.size()) out << ',';
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

  // Be tolerant of files saved with a UTF-8 BOM (common on Windows).
  if (text.size() >= 3 &&
      static_cast<unsigned char>(text[0]) == 0xEF &&
      static_cast<unsigned char>(text[1]) == 0xBB &&
      static_cast<unsigned char>(text[2]) == 0xBF) {
    p.i = 3;
  }

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
