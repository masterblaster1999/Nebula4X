#include "nebula4x/core/simulation.h"

#include <algorithm>
#include <cmath>

namespace nebula4x {
namespace {

double clamp01(double x) {
  if (!std::isfinite(x)) return 1.0;
  return std::clamp(x, 0.0, 1.0);
}

} // namespace

double Simulation::ship_subsystem_engine_multiplier(const Ship& ship) const {
  // Integrity effects are always applied; SimConfig::enable_ship_subsystem_damage controls
  // whether *combat* can inflict subsystem damage.
  return clamp01(ship.engines_integrity);
}

double Simulation::ship_subsystem_weapon_output_multiplier(const Ship& ship) const {
  return clamp01(ship.weapons_integrity);
}

double Simulation::ship_subsystem_sensor_range_multiplier(const Ship& ship) const {
  return clamp01(ship.sensors_integrity);
}

double Simulation::ship_subsystem_shield_multiplier(const Ship& ship) const {
  return clamp01(ship.shields_integrity);
}

} // namespace nebula4x
