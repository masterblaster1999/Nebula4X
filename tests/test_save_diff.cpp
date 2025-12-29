#include <iostream>
#include <string>

#include "nebula4x/util/json.h"
#include "nebula4x/util/save_diff.h"

#define N4X_ASSERT(expr)                                                                            \
  do {                                                                                              \
    if (!(expr)) {                                                                                  \
      std::cerr << "ASSERT failed: " #expr " (" << __FILE__ << ":" << __LINE__ << ")\n"; \
      return 1;                                                                                     \
    }                                                                                               \
  } while (0)

int test_save_diff() {
  const std::string a = "{\"a\":1,\"b\":[1,2]}";
  const std::string b = "{\"a\":2,\"b\":[1,3],\"c\":true}";

  {
    const std::string txt = nebula4x::diff_saves_to_text(a, b, nebula4x::SaveDiffOptions{.max_changes = 20});
    N4X_ASSERT(txt.find("/a") != std::string::npos);
    N4X_ASSERT(txt.find("/b/1") != std::string::npos);
    N4X_ASSERT(txt.find("/c") != std::string::npos);
  }

  {
    const std::string j = nebula4x::diff_saves_to_json(a, b, nebula4x::SaveDiffOptions{.max_changes = 20});
    const auto v = nebula4x::json::parse(j);
    N4X_ASSERT(v.is_object());
    const auto& o = v.object();
    N4X_ASSERT(o.find("changes") != o.end());
    N4X_ASSERT(o.at("changes").is_array());

    bool saw_a = false;
    bool saw_b1 = false;
    bool saw_c = false;

    for (const auto& ch : o.at("changes").array()) {
      N4X_ASSERT(ch.is_object());
      const auto& co = ch.object();
      const auto it_path = co.find("path");
      if (it_path == co.end() || !it_path->second.is_string()) continue;
      const std::string p = it_path->second.string_value();
      if (p == "/a") saw_a = true;
      if (p == "/b/1") saw_b1 = true;
      if (p == "/c") saw_c = true;
    }

    N4X_ASSERT(saw_a);
    N4X_ASSERT(saw_b1);
    N4X_ASSERT(saw_c);
  }

  // RFC 6902 JSON Patch roundtrip: diff -> apply -> equals.
  {
    const std::string patch = nebula4x::diff_saves_to_json_patch(a, b, nebula4x::JsonPatchOptions{.indent = 0});
    const std::string applied = nebula4x::apply_json_patch(a, patch, nebula4x::JsonPatchApplyOptions{.indent = 0});

    const auto canon_applied = nebula4x::json::stringify(nebula4x::json::parse(applied), 0);
    const auto canon_b = nebula4x::json::stringify(nebula4x::json::parse(b), 0);
    N4X_ASSERT(canon_applied == canon_b);
  }

  // Array removal ordering: removals must be emitted from end->front to keep indices valid.
  {
    const std::string a2 = "{\"arr\":[1,2,3]}";
    const std::string b2 = "{\"arr\":[1]}";
    const std::string patch = nebula4x::diff_saves_to_json_patch(a2, b2, nebula4x::JsonPatchOptions{.indent = 0});
    const auto pv = nebula4x::json::parse(patch);
    N4X_ASSERT(pv.is_array());
    const auto& ops = pv.array();

    // Expect: remove /arr/2 then remove /arr/1 (order matters).
    bool saw_remove_2_then_1 = false;
    if (ops.size() >= 2 && ops[0].is_object() && ops[1].is_object()) {
      const auto& o0 = ops[0].object();
      const auto& o1 = ops[1].object();
      const std::string op0 = o0.at("op").string_value();
      const std::string p0 = o0.at("path").string_value();
      const std::string op1 = o1.at("op").string_value();
      const std::string p1 = o1.at("path").string_value();
      saw_remove_2_then_1 = (op0 == "remove" && p0 == "/arr/2" && op1 == "remove" && p1 == "/arr/1");
    }
    N4X_ASSERT(saw_remove_2_then_1);

    const std::string applied = nebula4x::apply_json_patch(a2, patch, nebula4x::JsonPatchApplyOptions{.indent = 0});
    const auto canon_applied = nebula4x::json::stringify(nebula4x::json::parse(applied), 0);
    const auto canon_b2 = nebula4x::json::stringify(nebula4x::json::parse(b2), 0);
    N4X_ASSERT(canon_applied == canon_b2);
  }

  // JSON Pointer escaping.
  {
    const std::string a3 = "{\"x/y\":{\"~t\":1}}";
    const std::string b3 = "{\"x/y\":{\"~t\":2}}";
    const std::string patch = nebula4x::diff_saves_to_json_patch(a3, b3, nebula4x::JsonPatchOptions{.indent = 0});
    const std::string applied = nebula4x::apply_json_patch(a3, patch, nebula4x::JsonPatchApplyOptions{.indent = 0});
    const auto canon_applied = nebula4x::json::stringify(nebula4x::json::parse(applied), 0);
    const auto canon_b3 = nebula4x::json::stringify(nebula4x::json::parse(b3), 0);
    N4X_ASSERT(canon_applied == canon_b3);
  }

  return 0;
}
