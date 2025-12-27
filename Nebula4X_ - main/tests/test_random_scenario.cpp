#include <algorithm>
#include <iostream>
#include <queue>
#include <string>
#include <unordered_set>

#include "nebula4x/core/serialization.h"
#include "nebula4x/core/scenario.h"

#define N4X_ASSERT(expr)                                                                            \
  do {                                                                                              \
    if (!(expr)) {                                                                                  \
      std::cerr << "ASSERT failed: " #expr " (" << __FILE__ << ":" << __LINE__ << ")\n";           \
      return 1;                                                                                     \
    }                                                                                               \
  } while (0)

namespace {

// Basic graph connectivity check over the jump network.
bool jump_network_connected(const nebula4x::GameState& s) {
  if (s.systems.empty()) return true;
  const nebula4x::Id start = (s.selected_system != nebula4x::kInvalidId) ? s.selected_system : s.systems.begin()->first;

  std::unordered_set<nebula4x::Id> visited;
  std::queue<nebula4x::Id> q;
  visited.insert(start);
  q.push(start);

  while (!q.empty()) {
    const auto cur = q.front();
    q.pop();
    const auto* sys = nebula4x::find_ptr(s.systems, cur);
    if (!sys) continue;
    for (const auto jp_id : sys->jump_points) {
      const auto* jp = nebula4x::find_ptr(s.jump_points, jp_id);
      if (!jp) continue;
      const auto* other = nebula4x::find_ptr(s.jump_points, jp->linked_jump_id);
      if (!other) continue;
      const auto next_sys = other->system_id;
      if (next_sys == nebula4x::kInvalidId) continue;
      if (visited.insert(next_sys).second) q.push(next_sys);
    }
  }

  return visited.size() == s.systems.size();
}

} // namespace

int test_random_scenario() {
  const std::uint32_t seed = 12345;
  const int n = 10;

  const auto s1 = nebula4x::make_random_scenario(seed, n);
  const auto s2 = nebula4x::make_random_scenario(seed, n);
  const auto s3 = nebula4x::make_random_scenario(seed + 1, n);

  // Deterministic generation for the same (seed,n).
  const std::string j1 = nebula4x::serialize_game_to_json(s1);
  const std::string j2 = nebula4x::serialize_game_to_json(s2);
  N4X_ASSERT(j1 == j2);

  // A different seed should (very likely) differ.
  const std::string j3 = nebula4x::serialize_game_to_json(s3);
  N4X_ASSERT(j1 != j3);

  // Basic invariants.
  N4X_ASSERT(static_cast<int>(s1.systems.size()) == n);
  N4X_ASSERT(!s1.bodies.empty());
  N4X_ASSERT(!s1.colonies.empty());
  N4X_ASSERT(!s1.ships.empty());
  N4X_ASSERT(!s1.jump_points.empty());

  // Jump points should be bi-directionally linked.
  for (const auto& [id, jp] : s1.jump_points) {
    const auto* other = nebula4x::find_ptr(s1.jump_points, jp.linked_jump_id);
    N4X_ASSERT(other != nullptr);
    N4X_ASSERT(other->linked_jump_id == id);
    N4X_ASSERT(nebula4x::find_ptr(s1.systems, jp.system_id) != nullptr);
  }

  // ...and the graph should be connected.
  N4X_ASSERT(jump_network_connected(s1));

  return 0;
}
