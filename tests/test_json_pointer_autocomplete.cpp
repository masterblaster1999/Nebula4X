#include <iostream>
#include <string>
#include <vector>

#include "nebula4x/util/json.h"
#include "nebula4x/util/json_pointer_autocomplete.h"

#define N4X_ASSERT(expr) \
  do { \
    if (!(expr)) { \
      std::cerr << "ASSERT failed: " #expr " (" << __FILE__ << ":" << __LINE__ << ")\n"; \
      return 1; \
    } \
  } while (0)

static bool vec_contains(const std::vector<std::string>& v, const std::string& x) {
  for (const auto& s : v) {
    if (s == x) return true;
  }
  return false;
}

int test_json_pointer_autocomplete() {
  const std::string doc_txt = R"({
    "systems":[{"name":"Sol"},{"name":"Alpha"}],
    "ships":{"a":1,"b":2},
    "Weird/Key":{"~":5}
  })";

  nebula4x::json::Value doc = nebula4x::json::parse(doc_txt);

  // Root suggestions.
  {
    const auto sug = nebula4x::suggest_json_pointer_completions(doc, "/", 32, true, false);
    N4X_ASSERT(vec_contains(sug, "/systems"));
    N4X_ASSERT(vec_contains(sug, "/ships"));
    // Key with '/' should be escaped.
    N4X_ASSERT(vec_contains(sug, "/Weird~1Key"));
  }

  // Object key completion.
  {
    const auto sug = nebula4x::suggest_json_pointer_completions(doc, "/systems/0/n", 8, true, false);
    N4X_ASSERT(vec_contains(sug, "/systems/0/name"));
  }

  // Array index completion.
  {
    const auto sug = nebula4x::suggest_json_pointer_completions(doc, "/systems/1/", 8, true, false);
    N4X_ASSERT(vec_contains(sug, "/systems/1/name"));
  }

  // Escaping '~' key.
  {
    const auto sug = nebula4x::suggest_json_pointer_completions(doc, "/Weird~1Key/", 8, true, false);
    // "~" becomes "~0"
    N4X_ASSERT(vec_contains(sug, "/Weird~1Key/~0"));
  }

  return 0;
}
