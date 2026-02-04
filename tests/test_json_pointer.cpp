#include <iostream>
#include <limits>
#include <string>

#include "nebula4x/util/json.h"
#include "nebula4x/util/json_pointer.h"

#define N4X_ASSERT(expr) \
  do { \
    if (!(expr)) { \
      std::cerr << "ASSERT failed: " #expr " (" << __FILE__ << ":" << __LINE__ << ")\n"; \
      return 1; \
    } \
  } while (0)

int test_json_pointer() {
  using nebula4x::json::Value;

  // Escape/unescape.
  {
    const std::string token = "a/b~c";
    const std::string esc = nebula4x::json_pointer_escape_token(token);
    N4X_ASSERT(esc == "a~1b~0c");
    const std::string unesc = nebula4x::json_pointer_unescape_token(esc);
    N4X_ASSERT(unesc == token);
  }

  // Join.
  {
    N4X_ASSERT(nebula4x::json_pointer_join("/", "a") == "/a");
    N4X_ASSERT(nebula4x::json_pointer_join("/a", "b") == "/a/b");
    N4X_ASSERT(nebula4x::json_pointer_join_index("/a/b", 2) == "/a/b/2");
  }

  // Split.
  {
    const auto toks = nebula4x::split_json_pointer("/a~1b/c~0d", false);
    N4X_ASSERT(toks.size() == 2);
    N4X_ASSERT(toks[0] == "a/b");
    N4X_ASSERT(toks[1] == "c~d");

    const auto root1 = nebula4x::split_json_pointer("", false);
    N4X_ASSERT(root1.empty());

    const auto root2 = nebula4x::split_json_pointer("/", true);
    N4X_ASSERT(root2.empty());
  }

  // Resolve.
  {
    const std::string doc_txt = R"({"a":{"b":[10,{"c":"x"}]}})";
    Value doc = nebula4x::json::parse(doc_txt);

    std::string err;
    const Value* v = nebula4x::resolve_json_pointer(doc, "/a/b/1/c", true, &err);
    N4X_ASSERT(v != nullptr);
    N4X_ASSERT(v->is_string());
    N4X_ASSERT(v->string_value() == "x");

    const Value* root = nebula4x::resolve_json_pointer(doc, "/", true, &err);
    N4X_ASSERT(root != nullptr);
    N4X_ASSERT(root->is_object());

    const Value* missing = nebula4x::resolve_json_pointer(doc, "/nope", true, &err);
    N4X_ASSERT(missing == nullptr);
    N4X_ASSERT(!err.empty());

    // Leading zeros are rejected for array index tokens (except the single token "0").
    err.clear();
    const Value* leading_zero = nebula4x::resolve_json_pointer(doc, "/a/b/01", true, &err);
    N4X_ASSERT(leading_zero == nullptr);
    N4X_ASSERT(!err.empty());

    // An index token that doesn't fit in std::size_t must fail parse.
    const std::string huge = std::to_string(std::numeric_limits<std::size_t>::max()) + "0";
    err.clear();
    const Value* huge_idx = nebula4x::resolve_json_pointer(doc, "/a/b/" + huge, true, &err);
    N4X_ASSERT(huge_idx == nullptr);
    N4X_ASSERT(!err.empty());
  }

  return 0;
}
