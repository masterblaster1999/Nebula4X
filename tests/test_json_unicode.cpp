#include <iostream>
#include <string>

#include "nebula4x/util/json.h"

#define N4X_ASSERT(expr) \
  do { \
    if (!(expr)) { \
      std::cerr << "ASSERT failed: " #expr " (" << __FILE__ << ":" << __LINE__ << ")\n"; \
      return 1; \
    } \
  } while (0)

static bool parse_throws(const std::string& text) {
  try {
    (void)nebula4x::json::parse(text);
  } catch (...) {
    return true;
  }
  return false;
}

int test_json_unicode() {
  // Basic BMP codepoint (U+00E9, "é").
  {
    const auto v = nebula4x::json::parse(R"("\u00E9")");
    const auto* s = v.as_string();
    N4X_ASSERT(s != nullptr);
    N4X_ASSERT(*s == std::string("\xC3\xA9"));
  }

  // Surrogate pair (U+1F680, "🚀").
  {
    const auto v = nebula4x::json::parse(R"("\uD83D\uDE80")");
    const auto* s = v.as_string();
    N4X_ASSERT(s != nullptr);
    N4X_ASSERT(*s == std::string("\xF0\x9F\x9A\x80"));
  }

  // Round-trip through stringify (stringify emits UTF-8 bytes directly).
  {
    nebula4x::json::Array a;
    a.emplace_back(nebula4x::json::parse(R"("\u00E9")"));
    a.emplace_back(nebula4x::json::parse(R"("\uD83D\uDE80")"));

    const std::string dumped = nebula4x::json::stringify(nebula4x::json::array(std::move(a)), 0);
    const auto round = nebula4x::json::parse(dumped);
    const auto* arr = round.as_array();
    N4X_ASSERT(arr != nullptr);
    N4X_ASSERT(arr->size() == 2);

    N4X_ASSERT((*arr)[0].string_value() == std::string("\xC3\xA9"));
    N4X_ASSERT((*arr)[1].string_value() == std::string("\xF0\x9F\x9A\x80"));
  }

  // Invalid / incomplete surrogate sequences should fail.
  N4X_ASSERT(parse_throws(R"("\uD83D")"));
  N4X_ASSERT(parse_throws(R"("\uDE80")"));
  N4X_ASSERT(parse_throws(R"("\uD83D\u0041")"));
  N4X_ASSERT(parse_throws(R"("\uD83D\uFFFF")"));

  return 0;
}
