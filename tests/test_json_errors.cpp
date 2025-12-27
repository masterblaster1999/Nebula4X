#include <iostream>
#include <stdexcept>
#include <string>

#include "nebula4x/util/json.h"

#define N4X_ASSERT(expr) \
  do { \
    if (!(expr)) { \
      std::cerr << "ASSERT failed: " #expr " (" << __FILE__ << ":" << __LINE__ << ")\n"; \
      return 1; \
    } \
  } while (0)

static std::string parse_error_message(const std::string& text) {
  try {
    (void)nebula4x::json::parse(text);
  } catch (const std::runtime_error& e) {
    return std::string(e.what());
  } catch (...) {
    return "non-std::runtime_error";
  }
  return {};
}

int test_json_errors() {
  // The parser should include line/col and a context caret in error messages to make
  // hand-editing content JSON less painful.

  // Stray comma in an array.
  {
    const std::string msg = parse_error_message("[\n  1,\n  ,\n  2\n]\n");
    N4X_ASSERT(!msg.empty());
    N4X_ASSERT(msg.find("line 3, col 3") != std::string::npos);
    N4X_ASSERT(msg.find("unexpected") != std::string::npos);
    N4X_ASSERT(msg.find("^") != std::string::npos);
  }

  // Same stray comma case but with Windows CRLF line endings.
  {
    const std::string msg = parse_error_message("[\r\n  1,\r\n  ,\r\n  2\r\n]\r\n");
    N4X_ASSERT(!msg.empty());
    N4X_ASSERT(msg.find("line 3, col 3") != std::string::npos);
    N4X_ASSERT(msg.find("unexpected") != std::string::npos);
    N4X_ASSERT(msg.find("^") != std::string::npos);
  }

  // Missing closing brace at end-of-file.
  {
    const std::string msg = parse_error_message("{\n  \"a\": 1,\n  \"b\": 2");
    N4X_ASSERT(!msg.empty());
    N4X_ASSERT(msg.find("line 3, col 9") != std::string::npos);
    N4X_ASSERT(msg.find("expected") != std::string::npos);
    N4X_ASSERT(msg.find("^") != std::string::npos);
  }

  return 0;
}
