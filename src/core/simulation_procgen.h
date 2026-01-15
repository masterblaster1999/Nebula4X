#pragma once

// Internal procedural-generation helpers shared across simulation translation units.
//
// This header is intentionally located in src/ (not include/) so it does not
// become part of the public API. It provides deterministic RNG utilities and
// small helper routines used by procedural exploration systems.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "nebula4x/core/game_state.h"

namespace nebula4x::sim_procgen {

// Deterministic pseudo-random helper.
//
// splitmix64 is a fast, high-quality mixer suitable for deriving independent
// RNG streams from stable seeds like (day, system_id).
inline std::uint64_t splitmix64(std::uint64_t x) {
  x += 0x9e3779b97f4a7c15ULL;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
  return x ^ (x >> 31);
}

inline double u01_from_u64(std::uint64_t x) {
  // Use the top 53 bits to build a double in [0,1).
  const std::uint64_t v = x >> 11;
  return static_cast<double>(v) * (1.0 / 9007199254740992.0);  // 2^53
}

struct HashRng {
  std::uint64_t s{0};
  explicit HashRng(std::uint64_t seed) : s(seed) {}

  std::uint64_t next_u64() {
    s = splitmix64(s);
    return s;
  }

  double next_u01() { return u01_from_u64(next_u64()); }

  double range(double lo, double hi) {
    if (!(hi > lo)) return lo;
    return lo + (hi - lo) * next_u01();
  }

  int range_int(int lo, int hi_inclusive) {
    if (hi_inclusive <= lo) return lo;
    const int span = hi_inclusive - lo + 1;
    const double u = next_u01();
    int v = lo + static_cast<int>(u * static_cast<double>(span));
    if (v > hi_inclusive) v = hi_inclusive;
    return v;
  }
};

// Pick a plausible deep-space site position in a system.
//
// We bias toward the vicinity of a random jump point (if present) to make
// follow-up points-of-interest feel connected to interstellar travel lanes.
inline Vec2 pick_site_position_mkm(const GameState& s, Id system_id, HashRng& rng) {
  const auto* sys = find_ptr(s.systems, system_id);
  if (!sys) return Vec2{0.0, 0.0};

  Vec2 base{0.0, 0.0};
  if (!sys->jump_points.empty()) {
    std::vector<Id> jps = sys->jump_points;
    std::sort(jps.begin(), jps.end());
    const int idx = rng.range_int(0, static_cast<int>(jps.size()) - 1);
    if (const auto* jp = find_ptr(s.jump_points, jps[static_cast<std::size_t>(idx)])) base = jp->position_mkm;
  }

  const double ang = rng.range(0.0, 6.2831853071795864769);
  const double r = rng.range(25.0, 140.0);
  return base + Vec2{std::cos(ang) * r, std::sin(ang) * r};
}

inline std::unordered_map<std::string, double> generate_mineral_bundle(HashRng& rng, double scale) {
  static const char* pool[] = {"Duranium", "Neutronium", "Sorium", "Corbomite", "Tritanium"};
  constexpr int n = static_cast<int>(sizeof(pool) / sizeof(pool[0]));

  std::unordered_map<std::string, double> out;
  const int picks = rng.range_int(1, 3);
  for (int i = 0; i < picks; ++i) {
    const int idx = rng.range_int(0, n - 1);
    const double amt = std::max(0.0, scale) * rng.range(18.0, 95.0);
    out[pool[idx]] += amt;
  }

  // Prune tiny entries.
  for (auto it = out.begin(); it != out.end();) {
    if (!(it->second > 1e-6) || !std::isfinite(it->second)) {
      it = out.erase(it);
    } else {
      ++it;
    }
  }
  return out;
}

// Pick a component the given faction has not yet unlocked.
inline std::string pick_unlock_component_id(const ContentDB& content, const Faction& fac, HashRng& rng) {
  if (content.components.empty()) return {};

  std::unordered_set<std::string> unlocked;
  unlocked.reserve(fac.unlocked_components.size() * 2 + 8);
  for (const auto& cid : fac.unlocked_components) unlocked.insert(cid);

  std::vector<std::string> candidates;
  candidates.reserve(content.components.size());
  for (const auto& [cid, def] : content.components) {
    (void)def;
    if (cid.empty()) continue;
    if (unlocked.count(cid)) continue;
    candidates.push_back(cid);
  }
  if (candidates.empty()) return {};

  std::sort(candidates.begin(), candidates.end());
  const int idx = rng.range_int(0, static_cast<int>(candidates.size()) - 1);
  return candidates[static_cast<std::size_t>(idx)];
}

// Pick any component id from the content database (no faction filtering).
//
// Used for procedural points-of-interest where the investigating faction is not
// known at spawn time. Resolution-time logic will ignore already-known components.
inline std::string pick_any_component_id(const ContentDB& content, HashRng& rng) {
  if (content.components.empty()) return {};

  std::vector<std::string> ids;
  ids.reserve(content.components.size());
  for (const auto& [cid, def] : content.components) {
    (void)def;
    if (!cid.empty()) ids.push_back(cid);
  }
  if (ids.empty()) return {};
  std::sort(ids.begin(), ids.end());
  const int idx = rng.range_int(0, static_cast<int>(ids.size()) - 1);
  return ids[static_cast<std::size_t>(idx)];
}

}  // namespace nebula4x::sim_procgen
