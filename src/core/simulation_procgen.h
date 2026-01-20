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
#include "nebula4x/util/hash_rng.h"

namespace nebula4x::sim_procgen {

// Deterministic pseudo-random helper.
//
// We centralize the SplitMix64 mixer + HashRng implementation in nebula4x::util
// so simulation/procgen/AI all share identical RNG behavior.
using ::nebula4x::util::splitmix64;
using ::nebula4x::util::u01_from_u64;
using HashRng = ::nebula4x::util::HashRng;

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

// Pick any known component id from content.
//
// Used when procedural generation needs a plausible reward without faction
// context (for example, anomalies that exist before any faction discovers
// them).
//
// Returns an empty string if there are no components.
inline std::string pick_any_component_id(const ContentDB& content, HashRng& rng) {
  if (content.components.empty()) return {};

  std::vector<std::string> candidates;
  candidates.reserve(content.components.size());
  for (const auto& [cid, def] : content.components) {
    (void)def;
    if (cid.empty()) continue;
    candidates.push_back(cid);
  }
  if (candidates.empty()) return {};

  std::sort(candidates.begin(), candidates.end());
  const int idx = rng.range_int(0, static_cast<int>(candidates.size()) - 1);
  return candidates[static_cast<std::size_t>(idx)];
}

} // namespace nebula4x::sim_procgen
