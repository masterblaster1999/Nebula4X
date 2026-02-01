#pragma once

#include <string>

#include "nebula4x/core/ids.h"
#include "nebula4x/core/orders.h"

namespace nebula4x {
class Simulation;
}

namespace nebula4x::ui {

// Convert an Order into a human-readable string for UI display.
//
// This resolves entity ids (bodies/colonies/ships/jump points/etc.) into names
// when possible. When fog_of_war is enabled, names for undiscovered systems and
// undetected ships are suppressed to avoid leaking information.
std::string order_to_ui_string(const Simulation& sim, const Order& order, Id viewer_faction_id,
                               bool fog_of_war);

// Returns a compact label for the ship's "next action" suitable for list views.
//
// Priority:
//  1) suspended queue (prefixed with "[Suspended]")
//  2) active queue front
//  3) repeat template front (prefixed with repeat count)
//  4) empty string when no actionable orders.
std::string ship_orders_first_action_label(const Simulation& sim, const ShipOrders* so,
                                           Id viewer_faction_id, bool fog_of_war);

// Draw a tooltip describing a ship's order state.
//
// Intended usage:
//   if (ImGui::IsItemHovered()) draw_ship_orders_tooltip(...);
//
// max_lines caps how many orders are printed per list to avoid huge tooltips.
void draw_ship_orders_tooltip(const Simulation& sim, const ShipOrders* so, Id viewer_faction_id,
                              bool fog_of_war, int max_lines = 16);

}  // namespace nebula4x::ui
