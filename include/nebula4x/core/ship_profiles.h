#pragma once

#include "nebula4x/core/entities.h"

namespace nebula4x {

// Fine-grained toggles for applying a ShipAutomationProfile.
//
// Ship profiles are intended as a QoL feature for bulk configuration and do not
// change any simulation rules by themselves.
struct ShipProfileApplyOptions {
  // Apply mission automation flags and related parameters (thresholds, filters).
  bool apply_automation{true};

  // Apply repair scheduling priority.
  bool apply_repair_priority{true};

  // Apply power policy (enabled subsystems + priority order).
  bool apply_power_policy{true};

  // Apply sensor mode (EMCON).
  bool apply_sensor_mode{true};

  // Apply tactical combat doctrine (AttackShip standoff behavior).
  bool apply_combat_doctrine{true};
};

// Capture a profile from a ship's current settings.
ShipAutomationProfile make_ship_profile_from_ship(const Ship& ship);

// Apply a profile to a ship according to the provided options.
void apply_ship_profile(Ship& ship, const ShipAutomationProfile& profile,
                        const ShipProfileApplyOptions& opt = ShipProfileApplyOptions{});

}  // namespace nebula4x
