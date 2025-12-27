#include <iostream>
#include <string>

#include "nebula4x/util/json.h"

#define N4X_ASSERT(expr)                                                                            \
  do {                                                                                              \
    if (!(expr)) {                                                                                  \
      std::cerr << "ASSERT failed: " #expr " (" << __FILE__ << ":" << __LINE__ << ")\n";           \
      return 1;                                                                                     \
    }                                                                                               \
  } while (0)

int test_json_bom() {
  // Many Windows editors can emit a UTF-8 BOM at the start of a file. Treat it as optional
  // so content/saves edited by hand don't mysteriously fail to parse.

  {
    std::string txt;
    txt += "\xEF\xBB\xBF";
    txt += "{\"a\": 1, \"b\": [true, null, \"ok\"]}";

    const auto v = nebula4x::json::parse(txt);
    N4X_ASSERT(v.is_object());
    N4X_ASSERT(v.at("a").int_value() == 1);
    N4X_ASSERT(v.at("b").is_array());
    N4X_ASSERT(v.at("b").array().size() == 3);
    N4X_ASSERT(v.at("b").array()[0].bool_value() == true);
    N4X_ASSERT(v.at("b").array()[1].is_null());
    N4X_ASSERT(v.at("b").array()[2].string_value() == "ok");
  }

  // BOM followed by whitespace should still work.
  {
    std::string txt;
    txt += "\xEF\xBB\xBF";
    txt += " \n\t{\"x\": 2}\n";
    const auto v = nebula4x::json::parse(txt);
    N4X_ASSERT(v.is_object());
    N4X_ASSERT(v.at("x").int_value() == 2);
  }

  return 0;
}
