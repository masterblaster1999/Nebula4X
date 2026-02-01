#pragma once

#include <vector>

#include "nebula4x/core/ids.h"
#include "nebula4x/core/orders.h"

namespace nebula4x {
class Simulation;
}

namespace nebula4x::ui {

// Options for fleet-level mission planner previews.
struct FleetPlanPreviewOptions {
  // Fog-of-war context. When fog_of_war is false, planning behaves omniscient.
  Id viewer_faction_id{kInvalidId};
  bool fog_of_war{false};

  // When true, orders are compiled per ship via Simulation::compile_orders_smart
  // (inserting TravelViaJump legs automatically).
  bool smart_apply{true};

  // When true, preview assumes orders are appended to each ship's existing queue.
  bool append_when_applying{true};

  // When true, routing for smart compile / transfer helpers is restricted to
  // systems discovered by viewer_faction_id.
  bool restrict_to_discovered{true};

  // Planner configuration.
  bool predict_orbits{true};
  bool simulate_refuel{true};
  int max_orders{512};

  // Safety cap for very large fleets.
  int max_ships{64};

  // Reserve highlighting: warn when the minimum fuel along the plan dips below
  // reserve_fraction * fuel_capacity.
  double reserve_fraction{0.10};
  bool highlight_reserve{true};

  // When true, the detailed plan view collapses consecutive TravelViaJump orders.
  bool collapse_jump_chains{true};
};

// Render a fleet-level plan preview.
//
// `id_suffix` must be unique for each call site to avoid ImGui ID collisions.
void draw_fleet_plan_preview(const Simulation& sim, Id fleet_id, const std::vector<Order>& orders_to_apply,
                            const FleetPlanPreviewOptions& opts, const char* id_suffix);

}  // namespace nebula4x::ui
