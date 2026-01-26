#include "nebula4x/util/strings.h"

#include <algorithm>
#include <cctype>

namespace nebula4x {

std::string to_lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return s;
}


std::string csv_escape(const std::string& s) {
  bool needs_quotes = false;
  for (unsigned char c : s) {
    if (c == ',' || c == '"' || c == '\n' || c == '\r') {
      needs_quotes = true;
      break;
    }
  }

  if (!needs_quotes) return s;

  std::string out;
  out.reserve(s.size() + 2);
  out.push_back('"');
  for (unsigned char c : s) {
    if (c == '"') {
      out.push_back('"');
      out.push_back('"');
    } else {
      out.push_back(static_cast<char>(c));
    }
  }
  out.push_back('"');
  return out;
}



bool contains_ci(std::string_view haystack, std::string_view needle) {
  if (needle.empty()) return true;
  const std::string h = to_lower(std::string(haystack));
  const std::string n = to_lower(std::string(needle));
  return h.find(n) != std::string::npos;
}

} // namespace nebula4x
