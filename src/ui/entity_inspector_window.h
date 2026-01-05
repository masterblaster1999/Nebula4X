#pragma once

#include "nebula4x/core/simulation.h"

#include "ui/ui_state.h"

namespace nebula4x::ui {

// Procedural UI: Entity Inspector (ID resolver + inbound reference finder).
void draw_entity_inspector_window(Simulation& sim, UIState& ui);

} // namespace nebula4x::ui
