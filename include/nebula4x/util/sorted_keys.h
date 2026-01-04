#pragma once

#include <algorithm>
#include <vector>

namespace nebula4x::util {

// Many core containers are stored as std::unordered_map for convenience.
// Iteration order of unordered_map is not specified, so relying on it can
// introduce cross-platform nondeterminism in UI ordering.
//
// Returns keys in sorted order so calling code can present stable results.
template <typename Map>
inline std::vector<typename Map::key_type> sorted_keys(const Map& m) {
  std::vector<typename Map::key_type> keys;
  keys.reserve(m.size());
  for (const auto& kv : m) keys.push_back(kv.first);
  std::sort(keys.begin(), keys.end());
  return keys;
}

}  // namespace nebula4x::util
