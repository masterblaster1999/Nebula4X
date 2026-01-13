#pragma once

#include "nebula4x/core/simulation.h"

#include "ui_state.h"

namespace nebula4x::ui {

// Ship Profiles window: define + manage ship automation presets.
//
// A profile captures a ship's:
//  - mission automation flags (explore/freight/mine/etc)
//  - sustainment automation thresholds (refuel/repair/rearm/tanker)
//  - sensor mode (EMCON)
//  - power policy
//  - combat doctrine
//
// Profiles can be applied to a single ship, the selected fleet, or all ships in a faction.
void draw_ship_profiles_window(Simulation& sim, UIState& ui, Id& selected_ship, Id& selected_colony,
                               Id& selected_body);

}  // namespace nebula4x::ui
