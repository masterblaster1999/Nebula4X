#pragma once

#include "nebula4x/core/simulation.h"

#include "ui/ui_state.h"

namespace nebula4x::ui {

class ProcRenderEngine;
class ProcParticleFieldEngine;
class ProcBodySpriteEngine;
class ProcIconSpriteEngine;
class ProcJumpPhenomenaSpriteEngine;
class ProcAnomalyPhenomenaSpriteEngine;
class ProcTrailEngine;
class ProcFlowFieldEngine;
class ProcGravityContourEngine;

void draw_system_map(Simulation& sim,
                     UIState& ui,
                     Id& selected_ship,
                     Id& selected_colony,
                     Id& selected_body,
                     double& zoom,
                     Vec2& pan,
                     ProcRenderEngine* proc_engine,
                     ProcParticleFieldEngine* particle_engine,
                     ProcBodySpriteEngine* body_sprites,
                     ProcIconSpriteEngine* icon_sprites,
                     ProcJumpPhenomenaSpriteEngine* jump_fx,
                     ProcAnomalyPhenomenaSpriteEngine* anomaly_fx,
                     ProcTrailEngine* trail_engine,
                     ProcFlowFieldEngine* flow_engine,
                     ProcGravityContourEngine* gravity_engine);

} // namespace nebula4x::ui
