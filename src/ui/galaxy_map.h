#pragma once

#include "nebula4x/core/simulation.h"

#include "ui/ui_state.h"

namespace nebula4x::ui {

class ProcRenderEngine;
class ProcParticleFieldEngine;
class ProcTerritoryFieldEngine;

// Draws a galaxy-level map of star systems and jump links.
// - Honors UIState fog-of-war and discovered systems when a viewer faction is available.
// - Left click selects a system.
void draw_galaxy_map(Simulation& sim,
                     UIState& ui,
                     Id& selected_ship,
                     double& zoom,
                     Vec2& pan,
                     ProcRenderEngine* proc_engine,
                     ProcParticleFieldEngine* particle_engine,
               ProcTerritoryFieldEngine* territory_engine);

} // namespace nebula4x::ui
