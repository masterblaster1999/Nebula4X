#pragma once

#include "ui/hud.h"

namespace nebula4x {
class Simulation;
}

namespace nebula4x::ui {

// Evaluates Watchboard alert rules and emits HUD toast notifications.
//
// This runs even when the Watchboard window is closed.
void update_watchboard_alert_toasts(const Simulation& sim, UIState& ui, HUDState& hud);

} // namespace nebula4x::ui
