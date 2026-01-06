#include <iostream>
#include <string>

#include "nebula4x/util/json.h"
#include "nebula4x/util/trace_events.h"

#define N4X_ASSERT(expr)                                                                            \
  do {                                                                                              \
    if (!(expr)) {                                                                                  \
      std::cerr << "ASSERT failed: " #expr " (" << __FILE__ << ":" << __LINE__ << ")\n";       \
      return 1;                                                                                     \
    }                                                                                               \
  } while (0)

int test_trace_events() {
  using nebula4x::json::Value;

  auto& rec = nebula4x::trace::TraceRecorder::instance();
  rec.clear();
  rec.start("nebula4x_tests");

  {
    NEBULA4X_TRACE_SCOPE("outer", "test");
    {
      NEBULA4X_TRACE_SCOPE("inner", "test");
    }
  }

  rec.stop();

  const Value doc = rec.to_json();
  N4X_ASSERT(doc.is_array());
  const auto& arr = doc.array();
  N4X_ASSERT(!arr.empty());

  bool saw_process_name = false;
  bool saw_outer = false;
  bool saw_inner = false;

  for (const auto& ev : arr) {
    N4X_ASSERT(ev.is_object());
    const auto& o = ev.object();

    const auto it_name = o.find("name");
    N4X_ASSERT(it_name != o.end());
    N4X_ASSERT(it_name->second.is_string());
    const std::string name = it_name->second.string_value();

    const auto it_ph = o.find("ph");
    N4X_ASSERT(it_ph != o.end());
    N4X_ASSERT(it_ph->second.is_string());
    const std::string ph = it_ph->second.string_value();

    // Basic required fields.
    N4X_ASSERT(o.find("pid") != o.end());
    N4X_ASSERT(o.find("tid") != o.end());

    if (name == "process_name") {
      N4X_ASSERT(ph == "M");
      const auto it_args = o.find("args");
      N4X_ASSERT(it_args != o.end());
      N4X_ASSERT(it_args->second.is_object());
      const auto& args = it_args->second.object();
      const auto it_pn = args.find("name");
      N4X_ASSERT(it_pn != args.end());
      N4X_ASSERT(it_pn->second.is_string());
      N4X_ASSERT(it_pn->second.string_value() == "nebula4x_tests");
      saw_process_name = true;
    }

    if (name == "outer") {
      N4X_ASSERT(ph == "X");
      N4X_ASSERT(o.find("ts") != o.end());
      N4X_ASSERT(o.find("dur") != o.end());
      saw_outer = true;
    }
    if (name == "inner") {
      N4X_ASSERT(ph == "X");
      N4X_ASSERT(o.find("ts") != o.end());
      N4X_ASSERT(o.find("dur") != o.end());
      saw_inner = true;
    }
  }

  N4X_ASSERT(saw_process_name);
  N4X_ASSERT(saw_outer);
  N4X_ASSERT(saw_inner);

  rec.clear();
  return 0;
}
