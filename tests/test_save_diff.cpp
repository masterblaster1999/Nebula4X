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

  return 0;
}
