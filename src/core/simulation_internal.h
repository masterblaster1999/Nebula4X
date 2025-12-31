#pragma once

// Internal helpers shared across Simulation translation units.
//
// This header is intentionally *not* installed as part of the public API.
// It exists to keep simulation.cpp maintainable and to improve incremental
// build times by allowing large Simulation methods to live in separate .cpp
// files without duplicating utility code.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "nebula4x/core/game_state.h"

namespace nebula4x::sim_internal {

inline constexpr double kTwoPi = 6.283185307179586476925286766559;

inline std::string ascii_to_lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return s;
}

template <typename T>
inline void push_unique(std::vector<T>& v, const T& x) {
  if (std::find(v.begin(), v.end(), x) == v.end()) v.push_back(x);
}

template <typename T>
inline bool vec_contains(const std::vector<T>& v, const T& x) {
  return std::find(v.begin(), v.end(), x) != v.end();
}

inline bool is_mining_installation(const InstallationDef& def) {
  if (def.mining) return true;
  // Back-compat heuristic: if the content didn't explicitly set the flag,
  // treat installations whose id contains "mine" and that produce minerals as miners.
  if (def.produces_per_day.empty()) return false;
  const std::string lid = ascii_to_lower(def.id);
  return lid.find("mine") != std::string::npos;
}

inline bool faction_has_tech(const Faction& f, const std::string& tech_id) {
  return std::find(f.known_techs.begin(), f.known_techs.end(), tech_id) != f.known_techs.end();
}

inline double mkm_per_day_from_speed(double speed_km_s, double seconds_per_day) {
  const double km_per_day = speed_km_s * seconds_per_day;
  return km_per_day / 1.0e6; // million km
}

// Many core containers are stored as std::unordered_map for convenience.
// Iteration order of unordered_map is not specified, so relying on it can
// introduce cross-platform nondeterminism.
template <typename Map>
inline std::vector<typename Map::key_type> sorted_keys(const Map& m) {
  std::vector<typename Map::key_type> keys;
  keys.reserve(m.size());
  for (const auto& [k, _] : m) keys.push_back(k);
  std::sort(keys.begin(), keys.end());
  return keys;
}

// Power allocation helpers.
//
// The core algorithm lives in nebula4x/core/power.h (public, used by both UI
// and simulation). We keep small wrappers here so simulation translation units
// can share the same helpers without duplicating implementation.
using nebula4x::PowerAllocation;
using nebula4x::ShipPowerPolicy;

inline PowerAllocation compute_power_allocation(const ShipDesign& d, const ShipPowerPolicy& policy) {
  return nebula4x::compute_power_allocation(d.power_generation, d.power_use_engines, d.power_use_shields,
                                            d.power_use_weapons, d.power_use_sensors, policy);
}

inline PowerAllocation compute_power_allocation(const ShipDesign& d) {
  return compute_power_allocation(d, ShipPowerPolicy{});
}

// --- Faction economy modifiers ---
//
// Tech effects can apply simple, faction-wide multipliers to economic outputs.
// This is intentionally lightweight so content authors can prototype
// "+10% mining" or "+15% research" style techs without needing new component
// defs.
//
// Supported tech effect encodings (case-insensitive):
//   {"type":"faction_output_bonus", "value":"mining", "amount":0.10}
//     -> multiplies mining output by (1 + amount)
//   {"type":"faction_output_multiplier", "value":"research", "amount":1.15}
//     -> multiplies research output by amount
//
// value can be one of:
//   "all", "mining", "industry", "research", "construction", "shipyard",
//   "terraforming", "troop_training".
struct FactionEconomyMultipliers {
  double mining{1.0};
  double industry{1.0};
  double research{1.0};
  double construction{1.0};
  double shipyard{1.0};
  double terraforming{1.0};
  double troop_training{1.0};
};

inline double clamp_factor(double f) {
  if (!std::isfinite(f)) return 1.0;
  if (f < 0.0) return 0.0;
  return f;
}

inline void apply_factor(FactionEconomyMultipliers& m, const std::string& key, double factor) {
  if (key.empty() || key == "all") {
    m.mining *= factor;
    m.industry *= factor;
    m.research *= factor;
    m.construction *= factor;
    m.shipyard *= factor;
    m.terraforming *= factor;
    m.troop_training *= factor;
    return;
  }
  if (key == "mining") {
    m.mining *= factor;
    return;
  }
  if (key == "industry") {
    m.industry *= factor;
    return;
  }
  if (key == "research") {
    m.research *= factor;
    return;
  }
  if (key == "construction" || key == "construction_points" || key == "construction_point") {
    m.construction *= factor;
    return;
  }
  if (key == "shipyard") {
    m.shipyard *= factor;
    return;
  }
  if (key == "terraforming") {
    m.terraforming *= factor;
    return;
  }
  if (key == "troop_training" || key == "training") {
    m.troop_training *= factor;
    return;
  }
}

inline FactionEconomyMultipliers compute_faction_economy_multipliers(const ContentDB& content, const Faction& f) {
  FactionEconomyMultipliers out;
  for (const auto& tech_id : f.known_techs) {
    auto it = content.techs.find(tech_id);
    if (it == content.techs.end()) continue;
    const auto& tech = it->second;

    for (const auto& eff : tech.effects) {
      const std::string type = ascii_to_lower(eff.type);
      const std::string key = ascii_to_lower(eff.value);

      double factor = 1.0;
      if (type == "faction_output_bonus" || type == "faction_economy_bonus") {
        // bonus is expressed as a fraction (+0.10 == +10%).
        factor = 1.0 + eff.amount;
      } else if (type == "faction_output_multiplier" || type == "faction_economy_multiplier") {
        factor = eff.amount;
      } else {
        continue;
      }

      factor = clamp_factor(factor);
      if (factor <= 0.0) continue;
      apply_factor(out, key, factor);
    }
  }
  // Clamp any NaN/inf that slipped through.
  out.mining = clamp_factor(out.mining);
  out.industry = clamp_factor(out.industry);
  out.research = clamp_factor(out.research);
  out.construction = clamp_factor(out.construction);
  out.shipyard = clamp_factor(out.shipyard);
  out.terraforming = clamp_factor(out.terraforming);
  out.troop_training = clamp_factor(out.troop_training);
  return out;
}

inline std::uint64_t double_bits(double v) {
  std::uint64_t out = 0;
  static_assert(sizeof(out) == sizeof(v));
  std::memcpy(&out, &v, sizeof(out));
  return out;
}

} // namespace nebula4x::sim_internal
