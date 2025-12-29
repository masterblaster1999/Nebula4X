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

// Simple per-design power allocation used for load shedding.
//
// Priority order (highest to lowest): engines -> shields -> weapons -> sensors.
// If available reactor power is insufficient, lower-priority subsystems go
// offline. This is intentionally simple/deterministic for the prototype.
struct PowerAllocation {
  double generation{0.0};
  double available{0.0};
  bool engines_online{true};
  bool shields_online{true};
  bool weapons_online{true};
  bool sensors_online{true};
};

inline PowerAllocation compute_power_allocation(const ShipDesign& d) {
  PowerAllocation out;
  out.generation = std::max(0.0, d.power_generation);
  double avail = out.generation;

  auto on = [&](double req) {
    req = std::max(0.0, req);
    if (req <= 1e-9) return true;
    if (req <= avail + 1e-9) {
      avail -= req;
      return true;
    }
    return false;
  };

  out.engines_online = on(d.power_use_engines);
  out.shields_online = on(d.power_use_shields);
  out.weapons_online = on(d.power_use_weapons);
  out.sensors_online = on(d.power_use_sensors);

  out.available = avail;
  return out;
}

inline std::uint64_t double_bits(double v) {
  std::uint64_t out = 0;
  static_assert(sizeof(out) == sizeof(v));
  std::memcpy(&out, &v, sizeof(out));
  return out;
}

} // namespace nebula4x::sim_internal
