#pragma once

#include "nebula4x/core/simulation.h"

#include "ui_state.h"

namespace nebula4x::ui {

// Colony Profiles window: define + manage colony automation presets.
//
// A profile captures a colony's:
//  - installation targets (auto-build)
//  - mineral reserves/targets (auto-freight)
//  - garrison target (auto-training)
//
// You can apply profiles to a single colony or to every colony in a faction.
void draw_colony_profiles_window(Simulation& sim, UIState& ui, Id& selected_ship, Id& selected_colony,
                                 Id& selected_body);

}  // namespace nebula4x::ui
