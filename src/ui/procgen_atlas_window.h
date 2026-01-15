#pragma once

#include "nebula4x/core/ids.h"

namespace nebula4x {
class Simulation;
}

namespace nebula4x::ui {

struct UIState;

// ProcGen Atlas: in-game procedural generation analysis & visualization.
//
// This window is intended to make it easy to spot balance issues in the
// random scenario generator (resource clustering, nebula density cliffs, jump
// network chokepoints, etc.) while also being a fun "galaxy dossier".
void draw_procgen_atlas_window(Simulation& sim, UIState& ui, Id& selected_body);

} // namespace nebula4x::ui
