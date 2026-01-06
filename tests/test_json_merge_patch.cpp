#include <iostream>
#include <string>

#include "nebula4x/util/json.h"
#include "nebula4x/util/json_merge_patch.h"

#define N4X_ASSERT(expr)                                                                            \
  do {                                                                                              \
    if (!(expr)) {                                                                                  \
      std::cerr << "ASSERT failed: " #expr " (" << __FILE__ << ":" << __LINE__ << ")\n";          \
      return 1;                                                                                     \
    }                                                                                               \
  } while (0)

int test_json_merge_patch() {
  // Basic object merge patch: add/replace/remove and nested objects.
  {
    const std::string base = R"({"a":1,"b":{"x":1,"y":2},"c":[1,2]})";
    const std::string patch = R"({"a":2,"b":{"y":null,"z":3},"d":true})";
    const std::string want = R"({"a":2,"b":{"x":1,"z":3},"c":[1,2],"d":true})";

    const std::string got = nebula4x::apply_json_merge_patch(base, patch, /*indent=*/0);
    const std::string canon_got = nebula4x::json::stringify(nebula4x::json::parse(got), 0);
    const std::string canon_want = nebula4x::json::stringify(nebula4x::json::parse(want), 0);
    N4X_ASSERT(canon_got == canon_want);
  }

  // Non-object patch replaces the entire document.
  {
    const std::string base = R"({"a":1})";
    const std::string patch = R"([1,2,3])";
    const std::string want = R"([1,2,3])";

    const std::string got = nebula4x::apply_json_merge_patch(base, patch, /*indent=*/0);
    const std::string canon_got = nebula4x::json::stringify(nebula4x::json::parse(got), 0);
    const std::string canon_want = nebula4x::json::stringify(nebula4x::json::parse(want), 0);
    N4X_ASSERT(canon_got == canon_want);
  }

  // Object patch applied to a non-object base treats the base as an empty object.
  {
    const std::string base = R"(5)";
    const std::string patch = R"({"k":"v"})";
    const std::string want = R"({"k":"v"})";
    const std::string got = nebula4x::apply_json_merge_patch(base, patch, /*indent=*/0);
    const std::string canon_got = nebula4x::json::stringify(nebula4x::json::parse(got), 0);
    const std::string canon_want = nebula4x::json::stringify(nebula4x::json::parse(want), 0);
    N4X_ASSERT(canon_got == canon_want);
  }

  // Roundtrip: diff -> apply -> equals.
  {
    const std::string from = R"({"a":1,"b":{"x":1,"y":2}})";
    const std::string to = R"({"a":1,"b":{"x":2},"c":3})";

    const std::string patch = nebula4x::diff_json_merge_patch(from, to, /*indent=*/0);
    const std::string applied = nebula4x::apply_json_merge_patch(from, patch, /*indent=*/0);

    const std::string canon_applied = nebula4x::json::stringify(nebula4x::json::parse(applied), 0);
    const std::string canon_to = nebula4x::json::stringify(nebula4x::json::parse(to), 0);
    N4X_ASSERT(canon_applied == canon_to);

    // Ensure unchanged keys are omitted when possible.
    const auto pv = nebula4x::json::parse(patch);
    N4X_ASSERT(pv.is_object());
    const auto& po = pv.object();
    N4X_ASSERT(po.find("a") == po.end());
  }

  return 0;
}
