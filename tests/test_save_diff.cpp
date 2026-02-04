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

  // Array insertion near the front should prefer a single add over a cascade of replaces.
  {
    const std::string a2 = "{\"arr\":[1,2,3]}";
    const std::string b2 = "{\"arr\":[0,1,2,3]}";
    const std::string patch = nebula4x::diff_saves_to_json_patch(a2, b2, nebula4x::JsonPatchOptions{.indent = 0});

    const auto pv = nebula4x::json::parse(patch);
    N4X_ASSERT(pv.is_array());
    const auto& ops = pv.array();
    N4X_ASSERT(ops.size() == 1);
    N4X_ASSERT(ops[0].is_object());

    const auto& o0 = ops[0].object();
    N4X_ASSERT(o0.at("op").string_value() == "add");
    N4X_ASSERT(o0.at("path").string_value() == "/arr/0");
    N4X_ASSERT(o0.at("value").is_number());
    N4X_ASSERT(o0.at("value").number_value() == 0.0);

    const std::string applied = nebula4x::apply_json_patch(a2, patch, nebula4x::JsonPatchApplyOptions{.indent = 0});
    const auto canon_applied = nebula4x::json::stringify(nebula4x::json::parse(applied), 0);
    const auto canon_b2 = nebula4x::json::stringify(nebula4x::json::parse(b2), 0);
    N4X_ASSERT(canon_applied == canon_b2);
  }

  // Array insertion in the middle should also be represented as an add.
  {
    const std::string a2 = "{\"arr\":[1,2,3]}";
    const std::string b2 = "{\"arr\":[1,2,0,3]}";
    const std::string patch = nebula4x::diff_saves_to_json_patch(a2, b2, nebula4x::JsonPatchOptions{.indent = 0});

    const auto pv = nebula4x::json::parse(patch);
    N4X_ASSERT(pv.is_array());
    const auto& ops = pv.array();
    N4X_ASSERT(ops.size() == 1);
    N4X_ASSERT(ops[0].is_object());

    const auto& o0 = ops[0].object();
    N4X_ASSERT(o0.at("op").string_value() == "add");
    N4X_ASSERT(o0.at("path").string_value() == "/arr/2");
    N4X_ASSERT(o0.at("value").is_number());
    N4X_ASSERT(o0.at("value").number_value() == 0.0);

    const std::string applied = nebula4x::apply_json_patch(a2, patch, nebula4x::JsonPatchApplyOptions{.indent = 0});
    const auto canon_applied = nebula4x::json::stringify(nebula4x::json::parse(applied), 0);
    const auto canon_b2 = nebula4x::json::stringify(nebula4x::json::parse(b2), 0);
    N4X_ASSERT(canon_applied == canon_b2);
  }

  // Array deletion in the middle should be represented as a remove.
  {
    const std::string a2 = "{\"arr\":[1,0,2,3]}";
    const std::string b2 = "{\"arr\":[1,2,3]}";
    const std::string patch = nebula4x::diff_saves_to_json_patch(a2, b2, nebula4x::JsonPatchOptions{.indent = 0});

    const auto pv = nebula4x::json::parse(patch);
    N4X_ASSERT(pv.is_array());
    const auto& ops = pv.array();
    N4X_ASSERT(ops.size() == 1);
    N4X_ASSERT(ops[0].is_object());

    const auto& o0 = ops[0].object();
    N4X_ASSERT(o0.at("op").string_value() == "remove");
    N4X_ASSERT(o0.at("path").string_value() == "/arr/1");

    const std::string applied = nebula4x::apply_json_patch(a2, patch, nebula4x::JsonPatchApplyOptions{.indent = 0});
    const auto canon_applied = nebula4x::json::stringify(nebula4x::json::parse(applied), 0);
    const auto canon_b2 = nebula4x::json::stringify(nebula4x::json::parse(b2), 0);
    N4X_ASSERT(canon_applied == canon_b2);
  }
  // Array append at the end should use the special "-" index token.
  {
    const std::string a2 = R"({"arr":[1,2,3]})";
    const std::string b2 = R"({"arr":[1,2,3,4]})";
    const std::string patch = nebula4x::diff_saves_to_json_patch(a2, b2, nebula4x::JsonPatchOptions{.indent = 0});

    const auto pv = nebula4x::json::parse(patch);
    N4X_ASSERT(pv.is_array());
    const auto& ops = pv.array();
    N4X_ASSERT(ops.size() == 1);
    N4X_ASSERT(ops[0].is_object());

    const auto& o0 = ops[0].object();
    N4X_ASSERT(o0.at("op").string_value() == "add");
    N4X_ASSERT(o0.at("path").string_value() == "/arr/-");
    N4X_ASSERT(o0.at("value").is_number());
    N4X_ASSERT(o0.at("value").number_value() == 4.0);

    const std::string applied = nebula4x::apply_json_patch(a2, patch, nebula4x::JsonPatchApplyOptions{.indent = 0});
    const auto canon_applied = nebula4x::json::stringify(nebula4x::json::parse(applied), 0);
    const auto canon_b2 = nebula4x::json::stringify(nebula4x::json::parse(b2), 0);
    N4X_ASSERT(canon_applied == canon_b2);
  }

  // Insertion plus append: prefer an add at the insertion point and "-" for the final append.
  {
    const std::string a2 = R"({"arr":[1,2,3]})";
    const std::string b2 = R"({"arr":[1,2,0,3,4]})";
    const std::string patch = nebula4x::diff_saves_to_json_patch(a2, b2, nebula4x::JsonPatchOptions{.indent = 0});

    const auto pv = nebula4x::json::parse(patch);
    N4X_ASSERT(pv.is_array());
    const auto& ops = pv.array();
    N4X_ASSERT(ops.size() == 2);
    N4X_ASSERT(ops[0].is_object());
    N4X_ASSERT(ops[1].is_object());

    const auto& o0 = ops[0].object();
    const auto& o1 = ops[1].object();

    N4X_ASSERT(o0.at("op").string_value() == "add");
    N4X_ASSERT(o0.at("path").string_value() == "/arr/2");
    N4X_ASSERT(o0.at("value").is_number());
    N4X_ASSERT(o0.at("value").number_value() == 0.0);

    N4X_ASSERT(o1.at("op").string_value() == "add");
    N4X_ASSERT(o1.at("path").string_value() == "/arr/-");
    N4X_ASSERT(o1.at("value").is_number());
    N4X_ASSERT(o1.at("value").number_value() == 4.0);

    const std::string applied = nebula4x::apply_json_patch(a2, patch, nebula4x::JsonPatchApplyOptions{.indent = 0});
    const auto canon_applied = nebula4x::json::stringify(nebula4x::json::parse(applied), 0);
    const auto canon_b2 = nebula4x::json::stringify(nebula4x::json::parse(b2), 0);
    N4X_ASSERT(canon_applied == canon_b2);
  }





  // Array insertion plus a later element modification should stay concise (avoid replace cascades).
  {
    const std::string a2 = R"({"arr":[1,2,3,4,5]})";
    const std::string b2 = R"({"arr":[1,2,9,3,4,6]})";
    const std::string patch = nebula4x::diff_saves_to_json_patch(a2, b2, nebula4x::JsonPatchOptions{.indent = 0});

    const auto pv = nebula4x::json::parse(patch);
    N4X_ASSERT(pv.is_array());
    const auto& ops = pv.array();
    N4X_ASSERT(ops.size() == 2);
    N4X_ASSERT(ops[0].is_object());
    N4X_ASSERT(ops[1].is_object());

    const auto& o0 = ops[0].object();
    const auto& o1 = ops[1].object();

    N4X_ASSERT(o0.at("op").string_value() == "add");
    N4X_ASSERT(o0.at("path").string_value() == "/arr/2");
    N4X_ASSERT(o0.at("value").is_number());
    N4X_ASSERT(o0.at("value").number_value() == 9.0);

    N4X_ASSERT(o1.at("op").string_value() == "replace");
    N4X_ASSERT(o1.at("path").string_value() == "/arr/5");
    N4X_ASSERT(o1.at("value").is_number());
    N4X_ASSERT(o1.at("value").number_value() == 6.0);

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

  // Object member rename with identical value should be represented as a single move.
  {
    const std::string a3 = R"({"obj":{"old":123}})";
    const std::string b3 = R"({"obj":{"new":123}})";
    const std::string patch = nebula4x::diff_saves_to_json_patch(a3, b3, nebula4x::JsonPatchOptions{.indent = 0});

    const auto pv = nebula4x::json::parse(patch);
    N4X_ASSERT(pv.is_array());
    const auto& ops = pv.array();
    N4X_ASSERT(ops.size() == 1);
    N4X_ASSERT(ops[0].is_object());

    const auto& o0 = ops[0].object();
    N4X_ASSERT(o0.at("op").string_value() == "move");
    N4X_ASSERT(o0.at("from").string_value() == "/obj/old");
    N4X_ASSERT(o0.at("path").string_value() == "/obj/new");

    const std::string applied = nebula4x::apply_json_patch(a3, patch, nebula4x::JsonPatchApplyOptions{.indent = 0});
    const auto canon_applied = nebula4x::json::stringify(nebula4x::json::parse(applied), 0);
    const auto canon_b3 = nebula4x::json::stringify(nebula4x::json::parse(b3), 0);
    N4X_ASSERT(canon_applied == canon_b3);
  }



  // Object addition of a duplicate value should be emitted as a copy when safe.
  {
    const std::string a3 = R"({"obj":{"a":{"x":1,"y":2}}})";
    const std::string b3 = R"({"obj":{"a":{"x":1,"y":2},"b":{"x":1,"y":2}}})";
    const std::string patch = nebula4x::diff_saves_to_json_patch(a3, b3, nebula4x::JsonPatchOptions{.indent = 0});

    const auto pv = nebula4x::json::parse(patch);
    N4X_ASSERT(pv.is_array());
    const auto& ops = pv.array();
    N4X_ASSERT(ops.size() == 1);
    N4X_ASSERT(ops[0].is_object());

    const auto& o0 = ops[0].object();
    N4X_ASSERT(o0.at("op").string_value() == "copy");
    N4X_ASSERT(o0.at("from").string_value() == "/obj/a");
    N4X_ASSERT(o0.at("path").string_value() == "/obj/b");

    const std::string applied = nebula4x::apply_json_patch(a3, patch, nebula4x::JsonPatchApplyOptions{.indent = 0});
    const auto canon_applied = nebula4x::json::stringify(nebula4x::json::parse(applied), 0);
    const auto canon_b3 = nebula4x::json::stringify(nebula4x::json::parse(b3), 0);
    N4X_ASSERT(canon_applied == canon_b3);
  }

  // Object replacement with a duplicate value should be emitted as a copy when safe.
  {
    const std::string a3 = R"({"obj":{"a":{"x":1,"y":2},"b":{"x":9}}})";
    const std::string b3 = R"({"obj":{"a":{"x":1,"y":2},"b":{"x":1,"y":2}}})";
    const std::string patch = nebula4x::diff_saves_to_json_patch(a3, b3, nebula4x::JsonPatchOptions{.indent = 0});

    const auto pv = nebula4x::json::parse(patch);
    N4X_ASSERT(pv.is_array());
    const auto& ops = pv.array();
    N4X_ASSERT(ops.size() == 1);
    N4X_ASSERT(ops[0].is_object());

    const auto& o0 = ops[0].object();
    N4X_ASSERT(o0.at("op").string_value() == "copy");
    N4X_ASSERT(o0.at("from").string_value() == "/obj/a");
    N4X_ASSERT(o0.at("path").string_value() == "/obj/b");

    const std::string applied = nebula4x::apply_json_patch(a3, patch, nebula4x::JsonPatchApplyOptions{.indent = 0});
    const auto canon_applied = nebula4x::json::stringify(nebula4x::json::parse(applied), 0);
    const auto canon_b3 = nebula4x::json::stringify(nebula4x::json::parse(b3), 0);
    N4X_ASSERT(canon_applied == canon_b3);
  }

  // Object replacement can copy from a key that becomes stable earlier in the same object diff.
  {
    const std::string a3 = R"({"obj":{"a":{"x":1},"b":{"y":9}}})";
    const std::string b3 = R"({"obj":{"a":{"x":2},"b":{"x":2}}})";
    const std::string patch = nebula4x::diff_saves_to_json_patch(a3, b3, nebula4x::JsonPatchOptions{.indent = 0});

    const auto pv = nebula4x::json::parse(patch);
    N4X_ASSERT(pv.is_array());
    const auto& ops = pv.array();
    N4X_ASSERT(ops.size() == 2);
    N4X_ASSERT(ops[0].is_object());
    N4X_ASSERT(ops[1].is_object());

    const auto& o0 = ops[0].object();
    const auto& o1 = ops[1].object();

    N4X_ASSERT(o0.at("op").string_value() == "replace");
    N4X_ASSERT(o0.at("path").string_value() == "/obj/a/x");
    N4X_ASSERT(o0.at("value").is_number());
    N4X_ASSERT(o0.at("value").number_value() == 2.0);

    N4X_ASSERT(o1.at("op").string_value() == "copy");
    N4X_ASSERT(o1.at("from").string_value() == "/obj/a");
    N4X_ASSERT(o1.at("path").string_value() == "/obj/b");

    const std::string applied = nebula4x::apply_json_patch(a3, patch, nebula4x::JsonPatchApplyOptions{.indent = 0});
    const auto canon_applied = nebula4x::json::stringify(nebula4x::json::parse(applied), 0);
    const auto canon_b3 = nebula4x::json::stringify(nebula4x::json::parse(b3), 0);
    N4X_ASSERT(canon_applied == canon_b3);
  }

  // Array insertion/append of a duplicate value should be emitted as a copy when safe.
  {
    const std::string a3 = R"({"arr":[{"x":1},{"x":2}]})";
    const std::string b3 = R"({"arr":[{"x":1},{"x":2},{"x":1}]})";
    const std::string patch = nebula4x::diff_saves_to_json_patch(a3, b3, nebula4x::JsonPatchOptions{.indent = 0});

    const auto pv = nebula4x::json::parse(patch);
    N4X_ASSERT(pv.is_array());
    const auto& ops = pv.array();
    N4X_ASSERT(ops.size() == 1);
    N4X_ASSERT(ops[0].is_object());

    const auto& o0 = ops[0].object();
    N4X_ASSERT(o0.at("op").string_value() == "copy");
    N4X_ASSERT(o0.at("from").string_value() == "/arr/0");
    N4X_ASSERT(o0.at("path").string_value() == "/arr/-");

    const std::string applied = nebula4x::apply_json_patch(a3, patch, nebula4x::JsonPatchApplyOptions{.indent = 0});
    const auto canon_applied = nebula4x::json::stringify(nebula4x::json::parse(applied), 0);
    const auto canon_b3 = nebula4x::json::stringify(nebula4x::json::parse(b3), 0);
    N4X_ASSERT(canon_applied == canon_b3);
  }

  // Array insertion (not append) of a duplicate scalar should also prefer copy when safe.
  {
    const std::string a3 = R"({"arr":[1,2,3]})";
    const std::string b3 = R"({"arr":[1,2,1,3]})";
    const std::string patch = nebula4x::diff_saves_to_json_patch(a3, b3, nebula4x::JsonPatchOptions{.indent = 0});

    const auto pv = nebula4x::json::parse(patch);
    N4X_ASSERT(pv.is_array());
    const auto& ops = pv.array();
    N4X_ASSERT(ops.size() == 1);
    N4X_ASSERT(ops[0].is_object());

    const auto& o0 = ops[0].object();
    N4X_ASSERT(o0.at("op").string_value() == "copy");
    N4X_ASSERT(o0.at("from").string_value() == "/arr/0");
    N4X_ASSERT(o0.at("path").string_value() == "/arr/2");

    const std::string applied = nebula4x::apply_json_patch(a3, patch, nebula4x::JsonPatchApplyOptions{.indent = 0});
    const auto canon_applied = nebula4x::json::stringify(nebula4x::json::parse(applied), 0);
    const auto canon_b3 = nebula4x::json::stringify(nebula4x::json::parse(b3), 0);
    N4X_ASSERT(canon_applied == canon_b3);
  }

  // RFC 6902: copy.
  {
    const std::string doc = "{\"a\":1}";
    const std::string patch = R"([{"op":"copy","from":"/a","path":"/b"}])";
    const std::string applied = nebula4x::apply_json_patch(doc, patch, nebula4x::JsonPatchApplyOptions{.indent = 0});
    const auto canon_applied = nebula4x::json::stringify(nebula4x::json::parse(applied), 0);
    const auto canon_expected = nebula4x::json::stringify(nebula4x::json::parse("{\"a\":1,\"b\":1}"), 0);
    N4X_ASSERT(canon_applied == canon_expected);
  }

  // RFC 6902: move.
  {
    const std::string doc = "{\"a\":1,\"b\":2}";
    const std::string patch = R"([{"op":"move","from":"/a","path":"/c"}])";
    const std::string applied = nebula4x::apply_json_patch(doc, patch, nebula4x::JsonPatchApplyOptions{.indent = 0});
    const auto canon_applied = nebula4x::json::stringify(nebula4x::json::parse(applied), 0);
    const auto canon_expected = nebula4x::json::stringify(nebula4x::json::parse("{\"b\":2,\"c\":1}"), 0);
    N4X_ASSERT(canon_applied == canon_expected);
  }

  // RFC 6902: move within arrays follows remove-then-add semantics.
  {
    const std::string doc = "{\"arr\":[1,2,3]}";
    const std::string patch = R"([{"op":"move","from":"/arr/0","path":"/arr/2"}])";
    const std::string applied = nebula4x::apply_json_patch(doc, patch, nebula4x::JsonPatchApplyOptions{.indent = 0});
    const auto canon_applied = nebula4x::json::stringify(nebula4x::json::parse(applied), 0);
    const auto canon_expected = nebula4x::json::stringify(nebula4x::json::parse("{\"arr\":[2,3,1]}"), 0);
    N4X_ASSERT(canon_applied == canon_expected);
  }

  // RFC 6902: test.
  {
    const std::string doc = "{\"a\":{\"b\":3}}";

    // success
    const std::string patch_ok = R"([{"op":"test","path":"/a/b","value":3}])";
    (void)nebula4x::apply_json_patch(doc, patch_ok, nebula4x::JsonPatchApplyOptions{.indent = 0});

    // failure
    const std::string patch_bad = R"([{"op":"test","path":"/a/b","value":4}])";
    bool threw = false;
    try {
      (void)nebula4x::apply_json_patch(doc, patch_bad, nebula4x::JsonPatchApplyOptions{.indent = 0});
    } catch (...) {
      threw = true;
    }
    N4X_ASSERT(threw);
  }


  // JSON Patch array index tokens: leading zeros are rejected (except the single token "0").
  {
    const std::string doc = "{\"arr\":[1,2,3]}";
    const std::string patch = R"([{"op":"replace","path":"/arr/01","value":9}])";

    bool threw = false;
    try {
      (void)nebula4x::apply_json_patch(doc, patch, nebula4x::JsonPatchApplyOptions{.indent = 0});
    } catch (...) {
      threw = true;
    }
    N4X_ASSERT(threw);
  }


  // JSON Patch diagnostics include the failing op index.
  {
    const std::string doc = R"({"a":1})";
    const std::string patch = R"([
      {"op":"replace","path":"/a","value":1},
      {"op":"replace","path":"/missing","value":2}
    ])";

    bool saw_index = false;
    try {
      (void)nebula4x::apply_json_patch(doc, patch, nebula4x::JsonPatchApplyOptions{.indent = 0});
    } catch (const std::exception& e) {
      const std::string msg = e.what();
      saw_index = (msg.find("op #1") != std::string::npos);
    }
    N4X_ASSERT(saw_index);
  }

  // Parse errors also include op indices.
  {
    const std::string doc = R"({})";
    const std::string patch = R"([
      {"op":"nope","path":"","value":1}
    ])";

    bool saw_index = false;
    try {
      (void)nebula4x::apply_json_patch(doc, patch, nebula4x::JsonPatchApplyOptions{.indent = 0});
    } catch (const std::exception& e) {
      const std::string msg = e.what();
      saw_index = (msg.find("op #0") != std::string::npos);
    }
    N4X_ASSERT(saw_index);
  }


  // Array shift where the middle section slides left by one should prefer remove+add over a cascade of replaces.
  {
    const std::string a2 = R"({"arr":[1,2,3,4]})";
    const std::string b2 = R"({"arr":[1,3,4,5]})";
    const std::string patch = nebula4x::diff_saves_to_json_patch(a2, b2, nebula4x::JsonPatchOptions{.indent = 0});

    const auto pv = nebula4x::json::parse(patch);
    N4X_ASSERT(pv.is_array());
    const auto& ops = pv.array();
    N4X_ASSERT(ops.size() == 2);
    N4X_ASSERT(ops[0].is_object());
    N4X_ASSERT(ops[1].is_object());

    const auto& o0 = ops[0].object();
    const auto& o1 = ops[1].object();
    N4X_ASSERT(o0.at("op").string_value() == "remove");
    N4X_ASSERT(o0.at("path").string_value() == "/arr/1");
    N4X_ASSERT(o1.at("op").string_value() == "add");
    N4X_ASSERT(o1.at("path").string_value() == "/arr/-");
    N4X_ASSERT(o1.at("value").is_number());
    N4X_ASSERT(o1.at("value").number_value() == 5.0);

    const std::string applied = nebula4x::apply_json_patch(a2, patch, nebula4x::JsonPatchApplyOptions{.indent = 0});
    const auto canon_applied = nebula4x::json::stringify(nebula4x::json::parse(applied), 0);
    const auto canon_b2 = nebula4x::json::stringify(nebula4x::json::parse(b2), 0);
    N4X_ASSERT(canon_applied == canon_b2);
  }

  // Array rotation left by one can be expressed as a single move.
  {
    const std::string a2 = R"({"arr":[1,2,3]})";
    const std::string b2 = R"({"arr":[2,3,1]})";
    const std::string patch = nebula4x::diff_saves_to_json_patch(a2, b2, nebula4x::JsonPatchOptions{.indent = 0});

    const auto pv = nebula4x::json::parse(patch);
    N4X_ASSERT(pv.is_array());
    const auto& ops = pv.array();
    N4X_ASSERT(ops.size() == 1);
    N4X_ASSERT(ops[0].is_object());

    const auto& o0 = ops[0].object();
    N4X_ASSERT(o0.at("op").string_value() == "move");
    N4X_ASSERT(o0.at("from").string_value() == "/arr/0");
    N4X_ASSERT(o0.at("path").string_value() == "/arr/2");

    const std::string applied = nebula4x::apply_json_patch(a2, patch, nebula4x::JsonPatchApplyOptions{.indent = 0});
    const auto canon_applied = nebula4x::json::stringify(nebula4x::json::parse(applied), 0);
    const auto canon_b2 = nebula4x::json::stringify(nebula4x::json::parse(b2), 0);
    N4X_ASSERT(canon_applied == canon_b2);
  }

  // Array rotation right by one can also be expressed as a single move.
  {
    const std::string a2 = R"({"arr":[1,2,3]})";
    const std::string b2 = R"({"arr":[3,1,2]})";
    const std::string patch = nebula4x::diff_saves_to_json_patch(a2, b2, nebula4x::JsonPatchOptions{.indent = 0});

    const auto pv = nebula4x::json::parse(patch);
    N4X_ASSERT(pv.is_array());
    const auto& ops = pv.array();
    N4X_ASSERT(ops.size() == 1);
    N4X_ASSERT(ops[0].is_object());

    const auto& o0 = ops[0].object();
    N4X_ASSERT(o0.at("op").string_value() == "move");
    N4X_ASSERT(o0.at("from").string_value() == "/arr/2");
    N4X_ASSERT(o0.at("path").string_value() == "/arr/0");

    const std::string applied = nebula4x::apply_json_patch(a2, patch, nebula4x::JsonPatchApplyOptions{.indent = 0});
    const auto canon_applied = nebula4x::json::stringify(nebula4x::json::parse(applied), 0);
    const auto canon_b2 = nebula4x::json::stringify(nebula4x::json::parse(b2), 0);
    N4X_ASSERT(canon_applied == canon_b2);
  }



  // A single relocated array element should be represented as a single move (e.g., swap).
  {
    const std::string a2 = R"({"arr":[1,2,3,4]})";
    const std::string b2 = R"({"arr":[1,3,2,4]})";
    const std::string patch = nebula4x::diff_saves_to_json_patch(a2, b2, nebula4x::JsonPatchOptions{.indent = 0});

    const auto pv = nebula4x::json::parse(patch);
    N4X_ASSERT(pv.is_array());
    const auto& ops = pv.array();
    N4X_ASSERT(ops.size() == 1);
    N4X_ASSERT(ops[0].is_object());

    const auto& o0 = ops[0].object();
    N4X_ASSERT(o0.at("op").string_value() == "move");
    N4X_ASSERT(o0.at("from").string_value() == "/arr/1");
    N4X_ASSERT(o0.at("path").string_value() == "/arr/2");

    const std::string applied = nebula4x::apply_json_patch(a2, patch, nebula4x::JsonPatchApplyOptions{.indent = 0});
    const auto canon_applied = nebula4x::json::stringify(nebula4x::json::parse(applied), 0);
    const auto canon_b2 = nebula4x::json::stringify(nebula4x::json::parse(b2), 0);
    N4X_ASSERT(canon_applied == canon_b2);
  }

  // Another relocation example: move a tail element into the middle.
  {
    const std::string a2 = R"({"arr":[1,2,3,4]})";
    const std::string b2 = R"({"arr":[1,4,2,3]})";
    const std::string patch = nebula4x::diff_saves_to_json_patch(a2, b2, nebula4x::JsonPatchOptions{.indent = 0});

    const auto pv = nebula4x::json::parse(patch);
    N4X_ASSERT(pv.is_array());
    const auto& ops = pv.array();
    N4X_ASSERT(ops.size() == 1);
    N4X_ASSERT(ops[0].is_object());

    const auto& o0 = ops[0].object();
    N4X_ASSERT(o0.at("op").string_value() == "move");
    N4X_ASSERT(o0.at("from").string_value() == "/arr/3");
    N4X_ASSERT(o0.at("path").string_value() == "/arr/1");

    const std::string applied = nebula4x::apply_json_patch(a2, patch, nebula4x::JsonPatchApplyOptions{.indent = 0});
    const auto canon_applied = nebula4x::json::stringify(nebula4x::json::parse(applied), 0);
    const auto canon_b2 = nebula4x::json::stringify(nebula4x::json::parse(b2), 0);
    N4X_ASSERT(canon_applied == canon_b2);
  }

  // Array relocation where the moved element is also modified at its destination.
  // Prefer: move + nested replace, not a replace cascade.
  {
    const std::string a2 = R"({"arr":[{"id":1,"v":0},{"id":2,"v":0},{"id":3,"v":0}]})";
    const std::string b2 = R"({"arr":[{"id":2,"v":0},{"id":1,"v":5},{"id":3,"v":0}]})";
    const std::string patch = nebula4x::diff_saves_to_json_patch(a2, b2, nebula4x::JsonPatchOptions{.indent = 0});

    const auto pv = nebula4x::json::parse(patch);
    N4X_ASSERT(pv.is_array());
    const auto& ops = pv.array();
    N4X_ASSERT(ops.size() == 2);
    N4X_ASSERT(ops[0].is_object());
    N4X_ASSERT(ops[1].is_object());

    const auto& o0 = ops[0].object();
    const auto& o1 = ops[1].object();
    N4X_ASSERT(o0.at("op").string_value() == "move");
    N4X_ASSERT(o0.at("from").string_value() == "/arr/0");
    N4X_ASSERT(o0.at("path").string_value() == "/arr/1");

    N4X_ASSERT(o1.at("op").string_value() == "replace");
    N4X_ASSERT(o1.at("path").string_value() == "/arr/1/v");
    N4X_ASSERT(o1.at("value").is_number());
    N4X_ASSERT(o1.at("value").number_value() == 5.0);

    const std::string applied = nebula4x::apply_json_patch(a2, patch, nebula4x::JsonPatchApplyOptions{.indent = 0});
    const auto canon_applied = nebula4x::json::stringify(nebula4x::json::parse(applied), 0);
    const auto canon_b2 = nebula4x::json::stringify(nebula4x::json::parse(b2), 0);
    N4X_ASSERT(canon_applied == canon_b2);
  }

  // Object rename + nested edit should prefer move + nested patching.
  {
    const std::string a2 = R"({"obj":{"old":{"x":1,"y":2}}})";
    const std::string b2 = R"({"obj":{"new":{"x":1,"y":3}}})";
    const std::string patch = nebula4x::diff_saves_to_json_patch(a2, b2, nebula4x::JsonPatchOptions{.indent = 0});

    const auto pv = nebula4x::json::parse(patch);
    N4X_ASSERT(pv.is_array());
    const auto& ops = pv.array();
    N4X_ASSERT(ops.size() == 2);
    N4X_ASSERT(ops[0].is_object());
    N4X_ASSERT(ops[1].is_object());

    const auto& o0 = ops[0].object();
    const auto& o1 = ops[1].object();
    N4X_ASSERT(o0.at("op").string_value() == "move");
    N4X_ASSERT(o0.at("from").string_value() == "/obj/old");
    N4X_ASSERT(o0.at("path").string_value() == "/obj/new");

    N4X_ASSERT(o1.at("op").string_value() == "replace");
    N4X_ASSERT(o1.at("path").string_value() == "/obj/new/y");
    N4X_ASSERT(o1.at("value").is_number());
    N4X_ASSERT(o1.at("value").number_value() == 3.0);

    const std::string applied = nebula4x::apply_json_patch(a2, patch, nebula4x::JsonPatchApplyOptions{.indent = 0});
    const auto canon_applied = nebula4x::json::stringify(nebula4x::json::parse(applied), 0);
    const auto canon_b2 = nebula4x::json::stringify(nebula4x::json::parse(b2), 0);
    N4X_ASSERT(canon_applied == canon_b2);
  }

  // JSON Patch generator: optional 'test' preconditions (emit_tests) for ops that depend on existing values.
  {
    const std::string a2 = R"({"a":1,"b":2})";
    const std::string b2 = R"({"a":1,"b":3})";

    nebula4x::JsonPatchOptions opt;
    opt.indent = 0;
    opt.emit_tests = true;

    const std::string patch = nebula4x::diff_saves_to_json_patch(a2, b2, opt);
    const auto pv = nebula4x::json::parse(patch);
    N4X_ASSERT(pv.is_array());
    const auto& ops = pv.array();
    N4X_ASSERT(ops.size() == 2);
    N4X_ASSERT(ops[0].is_object());
    N4X_ASSERT(ops[1].is_object());

    const auto& o0 = ops[0].object();
    const auto& o1 = ops[1].object();
    N4X_ASSERT(o0.at("op").string_value() == "test");
    N4X_ASSERT(o0.at("path").string_value() == "/b");
    N4X_ASSERT(o0.at("value").is_number());
    N4X_ASSERT(o0.at("value").number_value() == 2.0);

    N4X_ASSERT(o1.at("op").string_value() == "replace");
    N4X_ASSERT(o1.at("path").string_value() == "/b");
    N4X_ASSERT(o1.at("value").is_number());
    N4X_ASSERT(o1.at("value").number_value() == 3.0);

    const std::string applied = nebula4x::apply_json_patch(a2, patch, nebula4x::JsonPatchApplyOptions{.indent = 0});
    const auto canon_applied = nebula4x::json::stringify(nebula4x::json::parse(applied), 0);
    const auto canon_b2 = nebula4x::json::stringify(nebula4x::json::parse(b2), 0);
    N4X_ASSERT(canon_applied == canon_b2);

    // Apply to a mismatched base should fail due to the precondition.
    const std::string bad_base = R"({"a":1,"b":99})";
    bool threw = false;
    try {
      (void)nebula4x::apply_json_patch(bad_base, patch, nebula4x::JsonPatchApplyOptions{.indent = 0});
    } catch (...) {
      threw = true;
    }
    N4X_ASSERT(threw);
  }

  // emit_tests should also guard move/copy sources (not just replace/remove).
  {
    const std::string a2 = R"({"obj":{"old":{"x":1}}})";
    const std::string b2 = R"({"obj":{"new":{"x":1}}})";

    nebula4x::JsonPatchOptions opt;
    opt.indent = 0;
    opt.emit_tests = true;

    const std::string patch = nebula4x::diff_saves_to_json_patch(a2, b2, opt);
    const auto pv = nebula4x::json::parse(patch);
    N4X_ASSERT(pv.is_array());
    const auto& ops = pv.array();
    N4X_ASSERT(ops.size() == 2);
    N4X_ASSERT(ops[0].is_object());
    N4X_ASSERT(ops[1].is_object());

    const auto& o0 = ops[0].object();
    const auto& o1 = ops[1].object();

    N4X_ASSERT(o0.at("op").string_value() == "test");
    N4X_ASSERT(o0.at("path").string_value() == "/obj/old");
    N4X_ASSERT(o0.at("value").is_object());
    N4X_ASSERT(o0.at("value").object().at("x").number_value() == 1.0);

    N4X_ASSERT(o1.at("op").string_value() == "move");
    N4X_ASSERT(o1.at("from").string_value() == "/obj/old");
    N4X_ASSERT(o1.at("path").string_value() == "/obj/new");

    const std::string applied = nebula4x::apply_json_patch(a2, patch, nebula4x::JsonPatchApplyOptions{.indent = 0});
    const auto canon_applied = nebula4x::json::stringify(nebula4x::json::parse(applied), 0);
    const auto canon_b2 = nebula4x::json::stringify(nebula4x::json::parse(b2), 0);
    N4X_ASSERT(canon_applied == canon_b2);

    // Drifted source should fail due to the move source precondition.
    const std::string bad_base = R"({"obj":{"old":{"x":999}}})";
    bool threw = false;
    try {
      (void)nebula4x::apply_json_patch(bad_base, patch, nebula4x::JsonPatchApplyOptions{.indent = 0});
    } catch (...) {
      threw = true;
    }
    N4X_ASSERT(threw);
  }

  {
    const std::string a2 = R"({"o":{"a":{"x":1}}})";
    const std::string b2 = R"({"o":{"a":{"x":1},"b":{"x":1}}})";

    nebula4x::JsonPatchOptions opt;
    opt.indent = 0;
    opt.emit_tests = true;

    const std::string patch = nebula4x::diff_saves_to_json_patch(a2, b2, opt);
    const auto pv = nebula4x::json::parse(patch);
    N4X_ASSERT(pv.is_array());
    const auto& ops = pv.array();
    N4X_ASSERT(ops.size() == 2);
    N4X_ASSERT(ops[0].is_object());
    N4X_ASSERT(ops[1].is_object());

    const auto& o0 = ops[0].object();
    const auto& o1 = ops[1].object();

    N4X_ASSERT(o0.at("op").string_value() == "test");
    N4X_ASSERT(o0.at("path").string_value() == "/o/a");
    N4X_ASSERT(o0.at("value").is_object());
    N4X_ASSERT(o0.at("value").object().at("x").number_value() == 1.0);

    N4X_ASSERT(o1.at("op").string_value() == "copy");
    N4X_ASSERT(o1.at("from").string_value() == "/o/a");
    N4X_ASSERT(o1.at("path").string_value() == "/o/b");

    const std::string applied = nebula4x::apply_json_patch(a2, patch, nebula4x::JsonPatchApplyOptions{.indent = 0});
    const auto canon_applied = nebula4x::json::stringify(nebula4x::json::parse(applied), 0);
    const auto canon_b2 = nebula4x::json::stringify(nebula4x::json::parse(b2), 0);
    N4X_ASSERT(canon_applied == canon_b2);

    // Drifted source should fail due to the copy source precondition.
    const std::string bad_base = R"({"o":{"a":{"x":2}}})";
    bool threw = false;
    try {
      (void)nebula4x::apply_json_patch(bad_base, patch, nebula4x::JsonPatchApplyOptions{.indent = 0});
    } catch (...) {
      threw = true;
    }
    N4X_ASSERT(threw);
  }

  {
    const std::string a2 = R"({"arr":[{"x":1}]})";
    const std::string b2 = R"({"arr":[{"x":1},{"x":1}]})";

    nebula4x::JsonPatchOptions opt;
    opt.indent = 0;
    opt.emit_tests = true;

    const std::string patch = nebula4x::diff_saves_to_json_patch(a2, b2, opt);
    const auto pv = nebula4x::json::parse(patch);
    N4X_ASSERT(pv.is_array());
    const auto& ops = pv.array();
    N4X_ASSERT(ops.size() == 2);
    N4X_ASSERT(ops[0].is_object());
    N4X_ASSERT(ops[1].is_object());

    const auto& o0 = ops[0].object();
    const auto& o1 = ops[1].object();

    N4X_ASSERT(o0.at("op").string_value() == "test");
    N4X_ASSERT(o0.at("path").string_value() == "/arr/0");
    N4X_ASSERT(o0.at("value").is_object());
    N4X_ASSERT(o0.at("value").object().at("x").number_value() == 1.0);

    N4X_ASSERT(o1.at("op").string_value() == "copy");
    N4X_ASSERT(o1.at("from").string_value() == "/arr/0");
    N4X_ASSERT(o1.at("path").string_value() == "/arr/-");

    const std::string applied = nebula4x::apply_json_patch(a2, patch, nebula4x::JsonPatchApplyOptions{.indent = 0});
    const auto canon_applied = nebula4x::json::stringify(nebula4x::json::parse(applied), 0);
    const auto canon_b2 = nebula4x::json::stringify(nebula4x::json::parse(b2), 0);
    N4X_ASSERT(canon_applied == canon_b2);
  }

  return 0;
}
