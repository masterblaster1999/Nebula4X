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

} // namespace nebula4x
