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
    "c": {"nested": {"k": 6}},
    "d": {"alpha": 7, "alps": 8, "beta": 9, "a*b": 10, "a?c": 11},
    "e": [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12]
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


  // Segment glob patterns over object keys.
  {
    std::string err;
    nebula4x::JsonPointerQueryStats st;
    const auto matches = nebula4x::query_json_pointer_glob(doc, "/d/al*", true, 64, 10000, &st, &err);
    N4X_ASSERT(err.empty());
    N4X_ASSERT(matches.size() == 2);
    N4X_ASSERT(has_path(matches, "/d/alpha"));
    N4X_ASSERT(has_path(matches, "/d/alps"));
  }

  // '?' matches exactly one character.
  {
    std::string err;
    nebula4x::JsonPointerQueryStats st;
    const auto matches = nebula4x::query_json_pointer_glob(doc, "/d/a?ps", true, 64, 10000, &st, &err);
    N4X_ASSERT(err.empty());
    N4X_ASSERT(matches.size() == 1);
    N4X_ASSERT(matches[0].path == "/d/alps");
  }

  // Escaped '*' and '?' match literally.
  {
    std::string err;
    nebula4x::JsonPointerQueryStats st;
    const auto m1 = nebula4x::query_json_pointer_glob(doc, "/d/a\\*b", true, 64, 10000, &st, &err);
    N4X_ASSERT(err.empty());
    N4X_ASSERT(m1.size() == 1);
    N4X_ASSERT(m1[0].path == "/d/a*b");

    const auto m2 = nebula4x::query_json_pointer_glob(doc, "/d/a\\?c", true, 64, 10000, &st, &err);
    N4X_ASSERT(err.empty());
    N4X_ASSERT(m2.size() == 1);
    N4X_ASSERT(m2[0].path == "/d/a?c");
  }

  // Segment glob patterns over array indices (indices are matched as strings).
  {
    std::string err;
    nebula4x::JsonPointerQueryStats st;
    const auto matches = nebula4x::query_json_pointer_glob(doc, "/e/1*", true, 64, 10000, &st, &err);
    N4X_ASSERT(err.empty());
    N4X_ASSERT(matches.size() == 4);
    N4X_ASSERT(has_path(matches, "/e/1"));
    N4X_ASSERT(has_path(matches, "/e/10"));
    N4X_ASSERT(has_path(matches, "/e/11"));
    N4X_ASSERT(has_path(matches, "/e/12"));
  }

  return 0;
}
