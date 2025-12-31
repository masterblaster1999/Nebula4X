#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace nebula4x {

// --- ship power management ---
//
// Ship designs declare power generation + per-subsystem power draws.
// At runtime, ships can configure which subsystems are enabled and the
// priority order used when power is insufficient.
//
// This is intentionally lightweight/deterministic for the prototype.

enum class PowerSubsystem : std::uint8_t {
  Engines = 0,
  Shields = 1,
  Weapons = 2,
  Sensors = 3,
};

inline const char* power_subsystem_id(PowerSubsystem s) {
  switch (s) {
    case PowerSubsystem::Engines: return "engines";
    case PowerSubsystem::Shields: return "shields";
    case PowerSubsystem::Weapons: return "weapons";
    case PowerSubsystem::Sensors: return "sensors";
  }
  return "engines";
}

inline const char* power_subsystem_label(PowerSubsystem s) {
  switch (s) {
    case PowerSubsystem::Engines: return "Engines";
    case PowerSubsystem::Shields: return "Shields";
    case PowerSubsystem::Weapons: return "Weapons";
    case PowerSubsystem::Sensors: return "Sensors";
  }
  return "Engines";
}

namespace power_internal {
inline bool ieq(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    const char ca = a[i];
    const char cb = b[i];
    auto lower = [](char c) {
      if (c >= 'A' && c <= 'Z') return static_cast<char>(c - 'A' + 'a');
      return c;
    };
    if (lower(ca) != lower(cb)) return false;
  }
  return true;
}
}  // namespace power_internal

inline PowerSubsystem power_subsystem_from_string(std::string_view s) {
  using power_internal::ieq;
  if (ieq(s, "engines") || ieq(s, "engine")) return PowerSubsystem::Engines;
  if (ieq(s, "shields") || ieq(s, "shield")) return PowerSubsystem::Shields;
  if (ieq(s, "weapons") || ieq(s, "weapon")) return PowerSubsystem::Weapons;
  if (ieq(s, "sensors") || ieq(s, "sensor")) return PowerSubsystem::Sensors;
  return PowerSubsystem::Engines;
}

inline std::string power_subsystem_to_string(PowerSubsystem s) {
  return std::string(power_subsystem_id(s));
}

// Per-ship runtime power preferences.
//
// When power generation is insufficient to cover all enabled subsystems,
// the ship will shed load in *reverse* priority order by failing to allocate
// power to lower-priority subsystems.
struct ShipPowerPolicy {
  bool engines_enabled{true};
  bool shields_enabled{true};
  bool weapons_enabled{true};
  bool sensors_enabled{true};

  // Highest-priority subsystem is priority[0].
  std::array<PowerSubsystem, 4> priority{
      PowerSubsystem::Engines,
      PowerSubsystem::Shields,
      PowerSubsystem::Weapons,
      PowerSubsystem::Sensors,
  };
};

// Result of allocating a design's power generation to subsystems.
struct PowerAllocation {
  double generation{0.0};
  double available{0.0};
  bool engines_online{true};
  bool shields_online{true};
  bool weapons_online{true};
  bool sensors_online{true};
};

inline std::array<PowerSubsystem, 4> sanitize_power_priority(std::array<PowerSubsystem, 4> prio) {
  const std::array<PowerSubsystem, 4> def{
      PowerSubsystem::Engines,
      PowerSubsystem::Shields,
      PowerSubsystem::Weapons,
      PowerSubsystem::Sensors,
  };

  std::array<bool, 4> seen{false, false, false, false};
  std::array<PowerSubsystem, 4> out = def;
  std::size_t n = 0;

  auto idx = [](PowerSubsystem s) -> std::size_t {
    return static_cast<std::size_t>(static_cast<std::uint8_t>(s));
  };

  for (PowerSubsystem s : prio) {
    const std::size_t i = idx(s);
    if (i >= seen.size()) continue;
    if (seen[i]) continue;
    seen[i] = true;
    if (n < out.size()) out[n++] = s;
  }

  for (PowerSubsystem s : def) {
    const std::size_t i = idx(s);
    if (i >= seen.size()) continue;
    if (seen[i]) continue;
    seen[i] = true;
    if (n < out.size()) out[n++] = s;
  }

  return out;
}

inline void sanitize_power_policy(ShipPowerPolicy& p) {
  p.priority = sanitize_power_priority(p.priority);
}

inline PowerAllocation compute_power_allocation(double generation,
                                                double power_use_engines,
                                                double power_use_shields,
                                                double power_use_weapons,
                                                double power_use_sensors,
                                                const ShipPowerPolicy& policy = ShipPowerPolicy{}) {
  PowerAllocation out;
  out.generation = std::max(0.0, generation);
  double avail = out.generation;

  // Enabled flags gate subsystems regardless of power draw.
  out.engines_online = policy.engines_enabled;
  out.shields_online = policy.shields_enabled;
  out.weapons_online = policy.weapons_enabled;
  out.sensors_online = policy.sensors_enabled;

  const auto prio = sanitize_power_priority(policy.priority);

  auto consume = [&](double req, bool& online) {
    req = std::max(0.0, req);
    if (!online) return;
    if (req <= 1e-9) return;
    if (req <= avail + 1e-9) {
      avail -= req;
      return;
    }
    online = false;
  };

  for (PowerSubsystem s : prio) {
    switch (s) {
      case PowerSubsystem::Engines:
        consume(power_use_engines, out.engines_online);
        break;
      case PowerSubsystem::Shields:
        consume(power_use_shields, out.shields_online);
        break;
      case PowerSubsystem::Weapons:
        consume(power_use_weapons, out.weapons_online);
        break;
      case PowerSubsystem::Sensors:
        consume(power_use_sensors, out.sensors_online);
        break;
    }
  }

  out.available = avail;
  return out;
}

}  // namespace nebula4x
