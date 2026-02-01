#pragma once

#include <string>
#include <vector>

#include "nebula4x/core/ids.h"
#include "nebula4x/core/order_planner.h"
#include "nebula4x/core/orders.h"

namespace nebula4x {
class Simulation;
}

namespace nebula4x::ui {

struct OrderPlanRenderOptions {
  Id viewer_faction_id{kInvalidId};
  bool fog_of_war{false};

  // Safety guard for huge queues.
  int max_rows{256};

  bool show_system{true};
  bool show_position{false};
  bool show_note{true};

  // When true, consecutive TravelViaJump orders are collapsed into a single row
  // (a "jump chain") in the planner table and exports.
  bool collapse_jump_chains{false};
};

// Render an order plan as a UI table.
//
// `table_id` must be unique within the current ImGui ID scope.
void draw_order_plan_table(const Simulation& sim, const std::vector<Order>& queue, const OrderPlan& plan,
                           double fuel_capacity_tons, const OrderPlanRenderOptions& opts, const char* table_id);

// Export plan rows as CSV. Intended for clipboard export.
std::string order_plan_to_csv(const Simulation& sim, const std::vector<Order>& queue, const OrderPlan& plan,
                              const OrderPlanRenderOptions& opts);

// Export plan as JSON. Intended for clipboard export / tooling.
std::string order_plan_to_json(const Simulation& sim, const std::vector<Order>& queue, const OrderPlan& plan,
                               const OrderPlanRenderOptions& opts, int indent = 2);

} // namespace nebula4x::ui
