#pragma once

#include <vector>

#include "nebula4x/core/ids.h"
#include "nebula4x/core/vec2.h"

namespace nebula4x {

class Simulation;
struct Ship;
struct ShipDesign;


namespace sim_sensors {

struct SensorSource {
  Vec2 pos_mkm{0.0, 0.0};
  double range_mkm{0.0};

  // Optional: ship id that generated this sensor source.
  //
  // When set (non-kInvalidId), callers can recover the ship's velocity to
  // perform swept detection across sub-day ticks. Colony-based sensor sources
  // use kInvalidId.
  Id ship_id{kInvalidId};
};


// Maximum signature multiplier the sensor system expects to handle for spatial
// queries (used to choose a conservative query radius).
double max_signature_multiplier_for_detection(const Simulation& sim);

// Effective signature multiplier for a specific ship, including design stealth
// and runtime sensor mode (EMCON).
double effective_signature_multiplier(const Simulation& sim, const Ship& ship, const ShipDesign* design = nullptr);

// Sensor range for a ship when acting as a sensor source, including sensor mode
// multipliers and power-policy availability.
double sensor_range_mkm_with_mode(const Simulation& sim, const Ship& ship, const ShipDesign& design);


// Collect sensor sources for a faction within a specific system.
// Sources currently include:
// - Friendly ship sensors (subject to power policy / power availability)
// - Best sensor range among friendly colony installations in that system
std::vector<SensorSource> gather_sensor_sources(const Simulation& sim, Id faction_id, Id system_id);

// Returns true if any sensor source can detect a target position.
bool any_source_detects(const std::vector<SensorSource>& sources, const Vec2& target_pos,
                        double target_signature_multiplier = 1.0);

} // namespace sim_sensors
} // namespace nebula4x
