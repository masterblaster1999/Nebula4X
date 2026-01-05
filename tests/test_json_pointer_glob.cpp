#include <iostream>
#include <string>
#include <unordered_set>

#include "nebula4x/util/json.h"
#include "nebula4x/util/json_pointer.h"

#define N4X_ASSERT(expr) \
  do { \
    if (!(expr)) { \
      std::cerr << "ASSERT failed: " #expr " (" << __FILE__ << ":" << __LINE__ << ")\n"; \
      return 1; \
    } \
  } while (0)

namespace {

bool has_path(const std::vector<nebula4x::JsonPointerQueryMatch>& matches, const std::string& want) {
  for (const auto& m : matches) {
    if (m.path == want) return true;
  }
  return false;
}

} // namespace

int test_json_pointer_glob() {
  using nebula4x::json::Value;

  const std::string doc_txt = R"({
    "a": {"x": 1, "y": 2},
    "b": [ {"v": 3}, {"v": 4}, {"w": 5} ],
    "c": {"nested": {"k": 6}}
  })";

  Value doc = nebula4x::json::parse(doc_txt);

  // Single-segment wildcard over an object.
  {
    std::string err;
    nebula4x::JsonPointerQueryStats st;
    const auto matches = nebula4x::query_json_pointer_glob(doc, "/a/*", true, 64, 10000, &st, &err);
    N4X_ASSERT(err.empty());
    N4X_ASSERT(st.matches == (int)matches.size());
    N4X_ASSERT(matches.size() == 2);
    N4X_ASSERT(has_path(matches, "/a/x"));
    N4X_ASSERT(has_path(matches, "/a/y"));
  }

  // Wildcard over an array, then a specific key.
  {
    std::string err;
    nebula4x::JsonPointerQueryStats st;
    const auto matches = nebula4x::query_json_pointer_glob(doc, "/b/*/v", true, 64, 10000, &st, &err);
    N4X_ASSERT(err.empty());
    N4X_ASSERT(matches.size() == 2);
    N4X_ASSERT(matches[0].value && matches[0].value->is_number());
    N4X_ASSERT(matches[1].value && matches[1].value->is_number());
    const std::unordered_set<std::string> paths{matches[0].path, matches[1].path};
    N4X_ASSERT(paths.count("/b/0/v") == 1);
    N4X_ASSERT(paths.count("/b/1/v") == 1);
  }

  // Recursive descent.
  {
    std::string err;
    nebula4x::JsonPointerQueryStats st;
    const auto matches = nebula4x::query_json_pointer_glob(doc, "/**/k", true, 64, 10000, &st, &err);
    N4X_ASSERT(err.empty());
    N4X_ASSERT(matches.size() == 1);
    N4X_ASSERT(matches[0].path == "/c/nested/k");
    N4X_ASSERT(matches[0].value && matches[0].value->is_number());
    N4X_ASSERT(matches[0].value->number_value() == 6.0);
  }

  // Root query.
  {
    std::string err;
    nebula4x::JsonPointerQueryStats st;
    const auto matches = nebula4x::query_json_pointer_glob(doc, "/", true, 64, 10000, &st, &err);
    N4X_ASSERT(err.empty());
    N4X_ASSERT(matches.size() == 1);
    N4X_ASSERT(matches[0].path == "/");
    N4X_ASSERT(matches[0].value && matches[0].value->is_object());
  }

  // Invalid pointer syntax.
  {
    std::string err;
    nebula4x::JsonPointerQueryStats st;
    const auto matches = nebula4x::query_json_pointer_glob(doc, "a/b", false, 64, 10000, &st, &err);
    N4X_ASSERT(matches.empty());
    N4X_ASSERT(!err.empty());
  }

  // Match limit should be reflected in stats.
  {
    std::string err;
    nebula4x::JsonPointerQueryStats st;
    const auto matches = nebula4x::query_json_pointer_glob(doc, "/**", true, 3, 10000, &st, &err);
    N4X_ASSERT(err.empty());
    N4X_ASSERT(matches.size() == 3);
    N4X_ASSERT(st.hit_match_limit);
  }

  return 0;
}
