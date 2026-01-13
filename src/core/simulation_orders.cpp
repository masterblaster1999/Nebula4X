#include "nebula4x/core/simulation.h"

#include "nebula4x/core/contact_prediction.h"

#include "simulation_internal.h"

#include "simulation_nav_helpers.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstring>
#include <iomanip>
#include <limits>
#include <optional>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

#include "nebula4x/core/scenario.h"
#include "nebula4x/core/ai_economy.h"
#include "nebula4x/util/log.h"
#include "nebula4x/util/spatial_index.h"

namespace nebula4x {
namespace {
using sim_internal::kTwoPi;
using sim_internal::ascii_to_lower;
using sim_internal::is_mining_installation;
using sim_internal::mkm_per_day_from_speed;
using sim_internal::push_unique;
using sim_internal::vec_contains;
using sim_internal::sorted_keys;
using sim_internal::faction_has_tech;
using sim_internal::FactionEconomyMultipliers;
using sim_internal::compute_faction_economy_multipliers;
using sim_internal::compute_power_allocation;
using sim_internal::strongest_active_treaty_between;

using sim_nav::PredictedNavState;
using sim_nav::predicted_nav_state_after_queued_jumps;

const char* treaty_type_display_name(TreatyType t) {
  switch (t) {
    case TreatyType::Ceasefire: return "ceasefire";
    case TreatyType::NonAggressionPact: return "non-aggression pact";
    case TreatyType::Alliance: return "alliance";
    case TreatyType::TradeAgreement: return "trade agreement";
  }
  return "treaty";
}
} // namespace

bool Simulation::clear_orders(Id ship_id) {
  if (!find_ptr(state_.ships, ship_id)) return false;
  auto& so = state_.ship_orders[ship_id];
  so.queue.clear();
  so.repeat = false;
  so.repeat_count_remaining = 0;
  so.repeat_template.clear();
  return true;
}

bool Simulation::enable_order_repeat(Id ship_id, int repeat_count_remaining) {
  if (!find_ptr(state_.ships, ship_id)) return false;
  auto& so = state_.ship_orders[ship_id];
  if (so.queue.empty()) return false;
  so.repeat = true;
  if (repeat_count_remaining < -1) repeat_count_remaining = -1;
  so.repeat_count_remaining = repeat_count_remaining;
  so.repeat_template = so.queue;
  return true;
}

bool Simulation::update_order_repeat_template(Id ship_id) {
  if (!find_ptr(state_.ships, ship_id)) return false;
  auto& so = state_.ship_orders[ship_id];
  if (so.queue.empty()) return false;
  if (!so.repeat) {
    so.repeat_count_remaining = -1;
  }
  so.repeat = true;
  so.repeat_template = so.queue;
  return true;
}

bool Simulation::disable_order_repeat(Id ship_id) {
  if (!find_ptr(state_.ships, ship_id)) return false;
  auto& so = state_.ship_orders[ship_id];
  so.repeat = false;
  so.repeat_count_remaining = 0;
  so.repeat_template.clear();
  return true;
}

bool Simulation::stop_order_repeat_keep_template(Id ship_id) {
  if (!find_ptr(state_.ships, ship_id)) return false;
  auto& so = state_.ship_orders[ship_id];
  so.repeat = false;
  so.repeat_count_remaining = 0;
  return true;
}

bool Simulation::set_order_repeat_count(Id ship_id, int repeat_count_remaining) {
  if (!find_ptr(state_.ships, ship_id)) return false;
  auto& so = state_.ship_orders[ship_id];
  if (repeat_count_remaining < -1) repeat_count_remaining = -1;
  so.repeat_count_remaining = repeat_count_remaining;
  return true;
}

bool Simulation::enable_order_repeat_from_template(Id ship_id, int repeat_count_remaining) {
  if (!find_ptr(state_.ships, ship_id)) return false;
  auto& so = state_.ship_orders[ship_id];
  if (so.repeat_template.empty()) return false;

  so.repeat = true;
  if (repeat_count_remaining < -1) repeat_count_remaining = -1;
  so.repeat_count_remaining = repeat_count_remaining;

  if (so.queue.empty()) {
    // Immediately start a cycle.
    so.queue = so.repeat_template;
  }
  return true;
}

bool Simulation::cancel_current_order(Id ship_id) {
  if (!find_ptr(state_.ships, ship_id)) return false;
  auto it = state_.ship_orders.find(ship_id);
  if (it == state_.ship_orders.end() || it->second.queue.empty()) return false;
  it->second.queue.erase(it->second.queue.begin());
  return true;
}

bool Simulation::delete_queued_order(Id ship_id, int index) {
  if (!find_ptr(state_.ships, ship_id)) return false;
  auto it = state_.ship_orders.find(ship_id);
  if (it == state_.ship_orders.end()) return false;
  auto& q = it->second.queue;
  if (index < 0 || index >= static_cast<int>(q.size())) return false;
  q.erase(q.begin() + index);
  return true;
}

bool Simulation::duplicate_queued_order(Id ship_id, int index) {
  if (!find_ptr(state_.ships, ship_id)) return false;
  auto it = state_.ship_orders.find(ship_id);
  if (it == state_.ship_orders.end()) return false;
  auto& q = it->second.queue;
  if (index < 0 || index >= static_cast<int>(q.size())) return false;
  const Order copy = q[index];
  q.insert(q.begin() + index + 1, copy);
  return true;
}

bool Simulation::move_queued_order(Id ship_id, int from_index, int to_index) {
  if (!find_ptr(state_.ships, ship_id)) return false;
  auto it = state_.ship_orders.find(ship_id);
  if (it == state_.ship_orders.end()) return false;

  auto& q = it->second.queue;
  const int n = static_cast<int>(q.size());
  if (from_index < 0 || from_index >= n) return false;

  // Interpret to_index as the desired final index after the move.
  // Allow callers to pass n (or larger) to mean "move to end".
  to_index = std::max(0, std::min(to_index, n));
  if (to_index >= n) to_index = n - 1;
  if (from_index == to_index) return true;

  Order moved = q[from_index];
  q.erase(q.begin() + from_index);

  // Insert at the desired index in the reduced vector. insert() allows index == size (end).
  to_index = std::max(0, std::min(to_index, static_cast<int>(q.size())));
  q.insert(q.begin() + to_index, std::move(moved));
  return true;
}


// --- Colony production queue editing (UI convenience) ---

bool Simulation::delete_shipyard_order(Id colony_id, int index) {
  auto* colony = find_ptr(state_.colonies, colony_id);
  if (!colony) return false;
  auto& q = colony->shipyard_queue;
  if (index < 0 || index >= static_cast<int>(q.size())) return false;
  q.erase(q.begin() + index);
  return true;
}

bool Simulation::move_shipyard_order(Id colony_id, int from_index, int to_index) {
  auto* colony = find_ptr(state_.colonies, colony_id);
  if (!colony) return false;
  auto& q = colony->shipyard_queue;
  const int n = static_cast<int>(q.size());
  if (from_index < 0 || from_index >= n) return false;

  // Interpret to_index as the desired final index after the move.
  // Allow callers to pass n (or larger) to mean "move to end".
  to_index = std::max(0, std::min(to_index, n));
  if (to_index >= n) to_index = n - 1;
  if (from_index == to_index) return true;

  BuildOrder moved = q[from_index];
  q.erase(q.begin() + from_index);

  to_index = std::max(0, std::min(to_index, static_cast<int>(q.size())));
  q.insert(q.begin() + to_index, std::move(moved));
  return true;
}

bool Simulation::delete_construction_order(Id colony_id, int index, bool refund_minerals) {
  auto* colony = find_ptr(state_.colonies, colony_id);
  if (!colony) return false;
  auto& q = colony->construction_queue;
  if (index < 0 || index >= static_cast<int>(q.size())) return false;

  if (refund_minerals) {
    const auto& ord = q[static_cast<std::size_t>(index)];
    if (ord.minerals_paid && !ord.installation_id.empty()) {
      auto it = content_.installations.find(ord.installation_id);
      if (it != content_.installations.end()) {
        for (const auto& [mineral, cost] : it->second.build_costs) {
          if (cost <= 0.0) continue;
          colony->minerals[mineral] += cost;
        }
      }
    }
  }

  q.erase(q.begin() + index);
  return true;
}

bool Simulation::move_construction_order(Id colony_id, int from_index, int to_index) {
  auto* colony = find_ptr(state_.colonies, colony_id);
  if (!colony) return false;
  auto& q = colony->construction_queue;
  const int n = static_cast<int>(q.size());
  if (from_index < 0 || from_index >= n) return false;

  // Interpret to_index as the desired final index after the move.
  // Allow callers to pass n (or larger) to mean "move to end".
  to_index = std::max(0, std::min(to_index, n));
  if (to_index >= n) to_index = n - 1;
  if (from_index == to_index) return true;

  InstallationBuildOrder moved = q[from_index];
  q.erase(q.begin() + from_index);

  to_index = std::max(0, std::min(to_index, static_cast<int>(q.size())));
  q.insert(q.begin() + to_index, std::move(moved));
  return true;
}

namespace {

bool has_non_whitespace(const std::string& s) {
  return std::any_of(s.begin(), s.end(), [](unsigned char c) { return !std::isspace(c); });
}

} // namespace

bool Simulation::save_order_template(const std::string& name, const std::vector<Order>& orders,
                                    bool overwrite, std::string* error) {
  auto fail = [&](const std::string& msg) {
    if (error) *error = msg;
    return false;
  };

  if (!has_non_whitespace(name)) return fail("Template name cannot be empty");
  if (orders.empty()) return fail("Template orders cannot be empty");

  const bool exists = (state_.order_templates.find(name) != state_.order_templates.end());
  if (exists && !overwrite) return fail("Template already exists");

  state_.order_templates[name] = orders;
  return true;
}

bool Simulation::delete_order_template(const std::string& name) {
  return state_.order_templates.erase(name) > 0;
}

bool Simulation::rename_order_template(const std::string& old_name, const std::string& new_name,
                                       std::string* error) {
  auto fail = [&](const std::string& msg) {
    if (error) *error = msg;
    return false;
  };

  if (old_name == new_name) return true;
  if (!has_non_whitespace(old_name)) return fail("Old name cannot be empty");
  if (!has_non_whitespace(new_name)) return fail("New name cannot be empty");

  auto it = state_.order_templates.find(old_name);
  if (it == state_.order_templates.end()) return fail("Template not found");
  if (state_.order_templates.find(new_name) != state_.order_templates.end()) {
    return fail("A template with that name already exists");
  }

  state_.order_templates[new_name] = std::move(it->second);
  state_.order_templates.erase(it);
  return true;
}

const std::vector<Order>* Simulation::find_order_template(const std::string& name) const {
  auto it = state_.order_templates.find(name);
  if (it == state_.order_templates.end()) return nullptr;
  return &it->second;
}

std::vector<std::string> Simulation::order_template_names() const {
  std::vector<std::string> out;
  out.reserve(state_.order_templates.size());
  for (const auto& [k, _] : state_.order_templates) out.push_back(k);
  std::sort(out.begin(), out.end());
  return out;
}

bool Simulation::apply_order_template_to_ship(Id ship_id, const std::string& name, bool append) {
  const auto* tmpl = find_order_template(name);
  if (!tmpl) return false;
  if (!find_ptr(state_.ships, ship_id)) return false;

  if (!append) {
    clear_orders(ship_id);
  }

  auto& so = state_.ship_orders[ship_id];
  so.queue.insert(so.queue.end(), tmpl->begin(), tmpl->end());
  return true;
}

bool Simulation::apply_order_template_to_fleet(Id fleet_id, const std::string& name, bool append) {
  prune_fleets();
  const auto* fl = find_ptr(state_.fleets, fleet_id);
  if (!fl) return false;

  bool ok = true;
  for (Id sid : fl->ship_ids) {
    if (!apply_order_template_to_ship(sid, name, append)) ok = false;
  }
  return ok;
}

bool Simulation::apply_order_template_to_ship_smart(Id ship_id, const std::string& name, bool append,
                                                    bool restrict_to_discovered, std::string* error) {
  auto fail = [&](const std::string& msg) {
    if (error) *error = msg;
    return false;
  };

  const auto* tmpl = find_order_template(name);
  if (!tmpl) return fail("Template not found");

  const auto* ship = find_ptr(state_.ships, ship_id);
  if (!ship) return fail("Ship not found");

  // Start from the ship's predicted system after any queued jumps if we are appending.
  PredictedNavState nav = predicted_nav_state_after_queued_jumps(state_, ship_id, append);
  if (nav.system_id == kInvalidId) return fail("Invalid ship navigation state");

  std::vector<Order> compiled;
  compiled.reserve(tmpl->size() + 8);

  auto route_to_system = [&](Id required_system_id, std::optional<Vec2> goal_pos_mkm) {
    if (required_system_id == kInvalidId) return fail("Invalid required system id");
    if (required_system_id == nav.system_id) return true;

    const auto plan = plan_jump_route_cached(nav.system_id, nav.position_mkm, ship->faction_id,
                                             ship->speed_km_s, required_system_id, restrict_to_discovered,
                                             goal_pos_mkm);
    if (!plan) {
      return fail("No jump route available to required system");
    }

    // Enqueue the source-side jump ids and update predicted nav state.
    for (Id jid : plan->jump_ids) {
      const auto* jp = find_ptr(state_.jump_points, jid);
      if (!jp) return fail("Route contained an invalid jump point");
      if (jp->system_id != nav.system_id) return fail("Route jump point is not in the current predicted system");
      if (jp->linked_jump_id == kInvalidId) return fail("Route jump point is unlinked");
      const auto* dest = find_ptr(state_.jump_points, jp->linked_jump_id);
      if (!dest) return fail("Route jump point has invalid destination");
      if (dest->system_id == kInvalidId) return fail("Route jump point has invalid destination system");
      if (!find_ptr(state_.systems, dest->system_id)) return fail("Route destination system does not exist");

      compiled.push_back(TravelViaJump{jid});
      nav.system_id = dest->system_id;
      nav.position_mkm = dest->position_mkm;
    }

    return true;
  };

  // Compile the template into a queue, injecting any missing travel.

  // Helper to validate a body exists and returns its system.
  auto body_system = [&](Id body_id) -> std::optional<Id> {
    const auto* b = find_ptr(state_.bodies, body_id);
    if (!b) return std::nullopt;
    if (b->system_id == kInvalidId) return std::nullopt;
    if (!find_ptr(state_.systems, b->system_id)) return std::nullopt;
    return b->system_id;
  };

  // Helper to find the system of a colony (via its body).
  auto colony_system = [&](Id colony_id) -> std::optional<Id> {
    const auto* c = find_ptr(state_.colonies, colony_id);
    if (!c) return std::nullopt;
    return body_system(c->body_id);
  };

  // Helper to get a body's position (for goal-aware routing).
  auto body_pos = [&](Id body_id) -> std::optional<Vec2> {
    const auto* b = find_ptr(state_.bodies, body_id);
    if (!b) return std::nullopt;
    return b->position_mkm;
  };

  // Helper to get a colony's position (its body's position).
  auto colony_pos = [&](Id colony_id) -> std::optional<Vec2> {
    const auto* c = find_ptr(state_.colonies, colony_id);
    if (!c) return std::nullopt;
    return body_pos(c->body_id);
  };

  // Helper to get a ship's current position (best-effort; may be stale for fog-of-war uses).
  auto ship_pos = [&](Id target_ship_id) -> std::optional<Vec2> {
    const auto* sh = find_ptr(state_.ships, target_ship_id);
    if (!sh) return std::nullopt;
    return sh->position_mkm;
  };


  auto ship_system = [&](Id target_ship_id) -> std::optional<Id> {
    const auto* sh = find_ptr(state_.ships, target_ship_id);
    if (!sh) return std::nullopt;
    if (sh->system_id == kInvalidId) return std::nullopt;
    if (!find_ptr(state_.systems, sh->system_id)) return std::nullopt;
    return sh->system_id;
  };

  auto update_position_to_body = [&](Id body_id) {
    if (const auto* b = find_ptr(state_.bodies, body_id)) {
      if (b->system_id == nav.system_id) {
        nav.position_mkm = b->position_mkm;
      }
    }
  };

  auto update_position_to_colony = [&](Id colony_id) {
    const auto* c = find_ptr(state_.colonies, colony_id);
    if (!c) return;
    update_position_to_body(c->body_id);
  };

  for (const auto& ord : *tmpl) {
    // Figure out which system the ship must be in for this order to be valid.
    std::optional<Id> required_system;
    std::optional<Vec2> goal_pos_mkm;

    if (std::holds_alternative<MoveToBody>(ord)) {
      required_system = body_system(std::get<MoveToBody>(ord).body_id);
      if (!required_system) return fail("Template MoveToBody references an invalid body");
    } else if (std::holds_alternative<ColonizeBody>(ord)) {
      required_system = body_system(std::get<ColonizeBody>(ord).body_id);
      if (!required_system) return fail("Template ColonizeBody references an invalid body");
    } else if (std::holds_alternative<OrbitBody>(ord)) {
      required_system = body_system(std::get<OrbitBody>(ord).body_id);
      if (!required_system) return fail("Template OrbitBody references an invalid body");
    } else if (std::holds_alternative<LoadMineral>(ord)) {
      required_system = colony_system(std::get<LoadMineral>(ord).colony_id);
      if (!required_system) return fail("Template LoadMineral references an invalid colony");
    } else if (std::holds_alternative<UnloadMineral>(ord)) {
      required_system = colony_system(std::get<UnloadMineral>(ord).colony_id);
      if (!required_system) return fail("Template UnloadMineral references an invalid colony");
    } else if (std::holds_alternative<LoadTroops>(ord)) {
      required_system = colony_system(std::get<LoadTroops>(ord).colony_id);
      if (!required_system) return fail("Template LoadTroops references an invalid colony");
    } else if (std::holds_alternative<UnloadTroops>(ord)) {
      required_system = colony_system(std::get<UnloadTroops>(ord).colony_id);
      if (!required_system) return fail("Template UnloadTroops references an invalid colony");
    } else if (std::holds_alternative<LoadColonists>(ord)) {
      required_system = colony_system(std::get<LoadColonists>(ord).colony_id);
      if (!required_system) return fail("Template LoadColonists references an invalid colony");
    } else if (std::holds_alternative<UnloadColonists>(ord)) {
      required_system = colony_system(std::get<UnloadColonists>(ord).colony_id);
      if (!required_system) return fail("Template UnloadColonists references an invalid colony");
    } else if (std::holds_alternative<InvadeColony>(ord)) {
      required_system = colony_system(std::get<InvadeColony>(ord).colony_id);
      if (!required_system) return fail("Template InvadeColony references an invalid colony");
    } else if (std::holds_alternative<ScrapShip>(ord)) {
      required_system = colony_system(std::get<ScrapShip>(ord).colony_id);
      if (!required_system) return fail("Template ScrapShip references an invalid colony");
        } else if (std::holds_alternative<AttackShip>(ord)) {
      required_system = ship_system(std::get<AttackShip>(ord).target_ship_id);
      if (!required_system) return fail("Template AttackShip references an invalid target ship");
    } else if (std::holds_alternative<TransferCargoToShip>(ord)) {
      required_system = ship_system(std::get<TransferCargoToShip>(ord).target_ship_id);
      if (!required_system) return fail("Template TransferCargoToShip references an invalid target ship");
    } else if (std::holds_alternative<TransferFuelToShip>(ord)) {
      required_system = ship_system(std::get<TransferFuelToShip>(ord).target_ship_id);
      if (!required_system) return fail("Template TransferFuelToShip references an invalid target ship");
    } else if (std::holds_alternative<TransferTroopsToShip>(ord)) {
      required_system = ship_system(std::get<TransferTroopsToShip>(ord).target_ship_id);
      if (!required_system) return fail("Template TransferTroopsToShip references an invalid target ship");
    } else if (std::holds_alternative<TravelViaJump>(ord)) {
      const Id jid = std::get<TravelViaJump>(ord).jump_point_id;
      const auto* jp = find_ptr(state_.jump_points, jid);
      if (!jp) return fail("Template TravelViaJump references an invalid jump point");
      required_system = jp->system_id;
      if (!required_system || *required_system == kInvalidId) {
        return fail("Template TravelViaJump has an invalid source system");
      }
    } else if (std::holds_alternative<SurveyJumpPoint>(ord)) {
      const Id jid = std::get<SurveyJumpPoint>(ord).jump_point_id;
      const auto* jp = find_ptr(state_.jump_points, jid);
      if (!jp) return fail("Template SurveyJumpPoint references an invalid jump point");
      required_system = jp->system_id;
      if (!required_system || *required_system == kInvalidId) {
        return fail("Template SurveyJumpPoint has an invalid source system");
      }
    }

    // If we know the target position inside the required system, provide it so jump routing
    // can prefer a better destination entry jump point.
    if (std::holds_alternative<MoveToBody>(ord)) {
      goal_pos_mkm = body_pos(std::get<MoveToBody>(ord).body_id);
    } else if (std::holds_alternative<ColonizeBody>(ord)) {
      goal_pos_mkm = body_pos(std::get<ColonizeBody>(ord).body_id);
    } else if (std::holds_alternative<OrbitBody>(ord)) {
      goal_pos_mkm = body_pos(std::get<OrbitBody>(ord).body_id);
    } else if (std::holds_alternative<LoadMineral>(ord)) {
      goal_pos_mkm = colony_pos(std::get<LoadMineral>(ord).colony_id);
    } else if (std::holds_alternative<UnloadMineral>(ord)) {
      goal_pos_mkm = colony_pos(std::get<UnloadMineral>(ord).colony_id);
    } else if (std::holds_alternative<LoadTroops>(ord)) {
      goal_pos_mkm = colony_pos(std::get<LoadTroops>(ord).colony_id);
    } else if (std::holds_alternative<UnloadTroops>(ord)) {
      goal_pos_mkm = colony_pos(std::get<UnloadTroops>(ord).colony_id);
    } else if (std::holds_alternative<LoadColonists>(ord)) {
      goal_pos_mkm = colony_pos(std::get<LoadColonists>(ord).colony_id);
    } else if (std::holds_alternative<UnloadColonists>(ord)) {
      goal_pos_mkm = colony_pos(std::get<UnloadColonists>(ord).colony_id);
    } else if (std::holds_alternative<InvadeColony>(ord)) {
      goal_pos_mkm = colony_pos(std::get<InvadeColony>(ord).colony_id);
    } else if (std::holds_alternative<ScrapShip>(ord)) {
      goal_pos_mkm = colony_pos(std::get<ScrapShip>(ord).colony_id);
    } else if (std::holds_alternative<AttackShip>(ord)) {
      const auto& a = std::get<AttackShip>(ord);
      if (a.has_last_known) {
        goal_pos_mkm = a.last_known_position_mkm;
      } else {
        goal_pos_mkm = ship_pos(a.target_ship_id);
      }
    } else if (std::holds_alternative<TransferCargoToShip>(ord)) {
      goal_pos_mkm = ship_pos(std::get<TransferCargoToShip>(ord).target_ship_id);
    } else if (std::holds_alternative<TransferFuelToShip>(ord)) {
      goal_pos_mkm = ship_pos(std::get<TransferFuelToShip>(ord).target_ship_id);
    } else if (std::holds_alternative<TransferTroopsToShip>(ord)) {
      goal_pos_mkm = ship_pos(std::get<TransferTroopsToShip>(ord).target_ship_id);
    } else if (std::holds_alternative<SurveyJumpPoint>(ord)) {
      const Id jid = std::get<SurveyJumpPoint>(ord).jump_point_id;
      if (const auto* jp = find_ptr(state_.jump_points, jid)) {
        goal_pos_mkm = jp->position_mkm;
      }
    }

    if (required_system) {
      if (!route_to_system(*required_system, goal_pos_mkm)) return false;
    }

    // Enqueue the actual template order.
    compiled.push_back(ord);

    // Update predicted nav state based on the order.
    if (std::holds_alternative<MoveToPoint>(ord)) {
      nav.position_mkm = std::get<MoveToPoint>(ord).target_mkm;
    } else if (std::holds_alternative<MoveToBody>(ord)) {
      update_position_to_body(std::get<MoveToBody>(ord).body_id);
    } else if (std::holds_alternative<ColonizeBody>(ord)) {
      update_position_to_body(std::get<ColonizeBody>(ord).body_id);
    } else if (std::holds_alternative<OrbitBody>(ord)) {
      update_position_to_body(std::get<OrbitBody>(ord).body_id);
    } else if (std::holds_alternative<LoadMineral>(ord)) {
      update_position_to_colony(std::get<LoadMineral>(ord).colony_id);
    } else if (std::holds_alternative<UnloadMineral>(ord)) {
      update_position_to_colony(std::get<UnloadMineral>(ord).colony_id);
    } else if (std::holds_alternative<LoadTroops>(ord)) {
      update_position_to_colony(std::get<LoadTroops>(ord).colony_id);
    } else if (std::holds_alternative<UnloadTroops>(ord)) {
      update_position_to_colony(std::get<UnloadTroops>(ord).colony_id);
    } else if (std::holds_alternative<LoadColonists>(ord)) {
      update_position_to_colony(std::get<LoadColonists>(ord).colony_id);
    } else if (std::holds_alternative<UnloadColonists>(ord)) {
      update_position_to_colony(std::get<UnloadColonists>(ord).colony_id);
    } else if (std::holds_alternative<InvadeColony>(ord)) {
      update_position_to_colony(std::get<InvadeColony>(ord).colony_id);
    } else if (std::holds_alternative<ScrapShip>(ord)) {
      update_position_to_colony(std::get<ScrapShip>(ord).colony_id);
      // Scrapping removes the ship; any subsequent orders would be meaningless.
      break;
    } else if (std::holds_alternative<TravelViaJump>(ord)) {
      const Id jid = std::get<TravelViaJump>(ord).jump_point_id;
      const auto* jp = find_ptr(state_.jump_points, jid);
      if (!jp) return fail("Template TravelViaJump references an invalid jump point");
      if (jp->system_id != nav.system_id) {
        // nav.system_id should already match required_system.
        return fail("Template TravelViaJump is not in the predicted system after routing");
      }
      if (jp->linked_jump_id == kInvalidId) return fail("Template TravelViaJump uses an unlinked jump point");
      const auto* dest = find_ptr(state_.jump_points, jp->linked_jump_id);
      if (!dest) return fail("Template TravelViaJump has invalid destination");
      if (dest->system_id == kInvalidId) return fail("Template TravelViaJump has invalid destination system");
      if (!find_ptr(state_.systems, dest->system_id)) return fail("Template TravelViaJump destination system missing");
      nav.system_id = dest->system_id;
      nav.position_mkm = dest->position_mkm;
    } else if (std::holds_alternative<SurveyJumpPoint>(ord)) {
      const auto& sj = std::get<SurveyJumpPoint>(ord);
      const Id jid = sj.jump_point_id;
      const auto* jp = find_ptr(state_.jump_points, jid);
      if (!jp) return fail("Template SurveyJumpPoint references an invalid jump point");
      if (jp->system_id != nav.system_id) {
        // nav.system_id should already match required_system.
        return fail("Template SurveyJumpPoint is not in the predicted system after routing");
      }
      nav.position_mkm = jp->position_mkm;

      if (sj.transit_when_done) {
        if (jp->linked_jump_id == kInvalidId) return fail("Template SurveyJumpPoint uses an unlinked jump point");
        const auto* dest = find_ptr(state_.jump_points, jp->linked_jump_id);
        if (!dest) return fail("Template SurveyJumpPoint has invalid destination");
        if (dest->system_id == kInvalidId) return fail("Template SurveyJumpPoint has invalid destination system");
        if (!find_ptr(state_.systems, dest->system_id)) return fail("Template SurveyJumpPoint destination system missing");
        nav.system_id = dest->system_id;
        nav.position_mkm = dest->position_mkm;
      }
    } else if (std::holds_alternative<AttackShip>(ord)) {
      // Best-effort: update position to the current target snapshot if it's in the same system.
      const Id tid = std::get<AttackShip>(ord).target_ship_id;
      if (const auto* t = find_ptr(state_.ships, tid)) {
        if (t->system_id == nav.system_id) nav.position_mkm = t->position_mkm;
      }
    } else if (std::holds_alternative<TransferCargoToShip>(ord)) {
      const Id tid = std::get<TransferCargoToShip>(ord).target_ship_id;
      if (const auto* t = find_ptr(state_.ships, tid)) {
        if (t->system_id == nav.system_id) nav.position_mkm = t->position_mkm;
      }
    } else if (std::holds_alternative<TransferFuelToShip>(ord)) {
      const Id tid = std::get<TransferFuelToShip>(ord).target_ship_id;
      if (const auto* t = find_ptr(state_.ships, tid)) {
        if (t->system_id == nav.system_id) nav.position_mkm = t->position_mkm;
      }
    } else if (std::holds_alternative<TransferTroopsToShip>(ord)) {
      const Id tid = std::get<TransferTroopsToShip>(ord).target_ship_id;
      if (const auto* t = find_ptr(state_.ships, tid)) {
        if (t->system_id == nav.system_id) nav.position_mkm = t->position_mkm;
      }
    }
  }

  if (compiled.empty()) return fail("Template produced no orders");

  // Apply atomically after successful compilation.
  if (!append) {
    if (!clear_orders(ship_id)) return fail("Failed to clear orders");
  }

  auto& so = state_.ship_orders[ship_id];
  so.queue.insert(so.queue.end(), compiled.begin(), compiled.end());
  return true;
}

bool Simulation::apply_order_template_to_fleet_smart(Id fleet_id, const std::string& name, bool append,
                                                     bool restrict_to_discovered, std::string* error) {
  prune_fleets();
  const auto* fl = find_ptr(state_.fleets, fleet_id);
  if (!fl) {
    if (error) *error = "Fleet not found";
    return false;
  }

  bool ok_any = false;
  std::string last_err;
  for (Id sid : fl->ship_ids) {
    std::string err;
    if (apply_order_template_to_ship_smart(sid, name, append, restrict_to_discovered, &err)) {
      ok_any = true;
    } else {
      last_err = err;
    }
  }

  if (!ok_any && error) *error = last_err;
  return ok_any;
}

Id Simulation::create_fleet(Id faction_id, const std::string& name, const std::vector<Id>& ship_ids,
                            std::string* error) {
  prune_fleets();

  auto fail = [&](const std::string& msg) -> Id {
    if (error) *error = msg;
    return kInvalidId;
  };

  if (faction_id == kInvalidId) return fail("Invalid faction id");
  if (!find_ptr(state_.factions, faction_id)) return fail("Faction does not exist");
  if (ship_ids.empty()) return fail("No ships provided");

  std::vector<Id> members;
  members.reserve(ship_ids.size());

  for (Id sid : ship_ids) {
    if (sid == kInvalidId) return fail("Invalid ship id in list");
    const auto* sh = find_ptr(state_.ships, sid);
    if (!sh) return fail("Ship does not exist: " + std::to_string(static_cast<unsigned long long>(sid)));
    if (sh->faction_id != faction_id) {
      return fail("Ship belongs to a different faction: " + sh->name);
    }
    const Id existing = fleet_for_ship(sid);
    if (existing != kInvalidId) {
      return fail("Ship already belongs to fleet " + std::to_string(static_cast<unsigned long long>(existing)));
    }
    members.push_back(sid);
  }

  std::sort(members.begin(), members.end());
  members.erase(std::unique(members.begin(), members.end()), members.end());
  if (members.empty()) return fail("No valid ships provided");

  Fleet fl;
  fl.id = allocate_id(state_);
  fl.name = name.empty() ? ("Fleet " + std::to_string(static_cast<unsigned long long>(fl.id))) : name;
  fl.faction_id = faction_id;
  fl.ship_ids = members;
  fl.leader_ship_id = members.front();

  const Id fleet_id = fl.id;
  state_.fleets[fleet_id] = std::move(fl);
  return fleet_id;
}

bool Simulation::disband_fleet(Id fleet_id) {
  return state_.fleets.erase(fleet_id) > 0;
}

bool Simulation::add_ship_to_fleet(Id fleet_id, Id ship_id, std::string* error) {
  prune_fleets();

  auto fail = [&](const std::string& msg) {
    if (error) *error = msg;
    return false;
  };

  auto* fl = find_ptr(state_.fleets, fleet_id);
  if (!fl) return fail("Fleet does not exist");

  if (ship_id == kInvalidId) return fail("Invalid ship id");
  const auto* sh = find_ptr(state_.ships, ship_id);
  if (!sh) return fail("Ship does not exist");
  if (fl->faction_id != kInvalidId && sh->faction_id != fl->faction_id) {
    return fail("Ship faction does not match fleet faction");
  }

  const Id existing = fleet_for_ship(ship_id);
  if (existing != kInvalidId && existing != fleet_id) {
    return fail("Ship already belongs to fleet " + std::to_string(static_cast<unsigned long long>(existing)));
  }

  if (std::find(fl->ship_ids.begin(), fl->ship_ids.end(), ship_id) != fl->ship_ids.end()) {
    return true; // already in this fleet
  }

  fl->ship_ids.push_back(ship_id);
  std::sort(fl->ship_ids.begin(), fl->ship_ids.end());
  fl->ship_ids.erase(std::unique(fl->ship_ids.begin(), fl->ship_ids.end()), fl->ship_ids.end());
  if (fl->leader_ship_id == kInvalidId && !fl->ship_ids.empty()) fl->leader_ship_id = fl->ship_ids.front();
  return true;
}

bool Simulation::remove_ship_from_fleet(Id fleet_id, Id ship_id) {
  auto* fl = find_ptr(state_.fleets, fleet_id);
  if (!fl) return false;

  const auto it = std::remove(fl->ship_ids.begin(), fl->ship_ids.end(), ship_id);
  if (it == fl->ship_ids.end()) return false;
  fl->ship_ids.erase(it, fl->ship_ids.end());
  if (fl->leader_ship_id == ship_id) fl->leader_ship_id = kInvalidId;
  prune_fleets();
  return true;
}

bool Simulation::set_fleet_leader(Id fleet_id, Id ship_id) {
  auto* fl = find_ptr(state_.fleets, fleet_id);
  if (!fl) return false;
  if (ship_id == kInvalidId) return false;
  if (std::find(fl->ship_ids.begin(), fl->ship_ids.end(), ship_id) == fl->ship_ids.end()) return false;
  fl->leader_ship_id = ship_id;
  return true;
}

bool Simulation::rename_fleet(Id fleet_id, const std::string& name) {
  auto* fl = find_ptr(state_.fleets, fleet_id);
  if (!fl) return false;
  if (name.empty()) return false;
  fl->name = name;
  return true;
}

bool Simulation::configure_fleet_formation(Id fleet_id, FleetFormation formation, double spacing_mkm) {
  auto* fl = find_ptr(state_.fleets, fleet_id);
  if (!fl) return false;
  fl->formation = formation;
  fl->formation_spacing_mkm = std::max(0.0, spacing_mkm);
  return true;
}

Id Simulation::fleet_for_ship(Id ship_id) const {
  if (ship_id == kInvalidId) return kInvalidId;
  for (const auto& [fid, fl] : state_.fleets) {
    if (std::find(fl.ship_ids.begin(), fl.ship_ids.end(), ship_id) != fl.ship_ids.end()) return fid;
  }
  return kInvalidId;
}

bool Simulation::clear_fleet_orders(Id fleet_id) {
  prune_fleets();
  const auto* fl = find_ptr(state_.fleets, fleet_id);
  if (!fl) return false;

  bool any = false;
  for (Id sid : fl->ship_ids) {
    if (clear_orders(sid)) any = true;
  }
  return any;
}

bool Simulation::issue_fleet_wait_days(Id fleet_id, int days) {
  prune_fleets();
  const auto* fl = find_ptr(state_.fleets, fleet_id);
  if (!fl) return false;
  bool any = false;
  for (Id sid : fl->ship_ids) {
    if (issue_wait_days(sid, days)) any = true;
  }
  return any;
}

bool Simulation::issue_fleet_move_to_point(Id fleet_id, Vec2 target_mkm) {
  prune_fleets();
  const auto* fl = find_ptr(state_.fleets, fleet_id);
  if (!fl) return false;
  bool any = false;
  for (Id sid : fl->ship_ids) {
    if (issue_move_to_point(sid, target_mkm)) any = true;
  }
  return any;
}

bool Simulation::issue_fleet_move_to_body(Id fleet_id, Id body_id, bool restrict_to_discovered) {
  prune_fleets();
  const auto* fl = find_ptr(state_.fleets, fleet_id);
  if (!fl) return false;
  bool any = false;
  for (Id sid : fl->ship_ids) {
    if (issue_move_to_body(sid, body_id, restrict_to_discovered)) any = true;
  }
  return any;
}

bool Simulation::issue_fleet_orbit_body(Id fleet_id, Id body_id, int duration_days, bool restrict_to_discovered) {
  prune_fleets();
  const auto* fl = find_ptr(state_.fleets, fleet_id);
  if (!fl) return false;
  bool any = false;
  for (Id sid : fl->ship_ids) {
    if (issue_orbit_body(sid, body_id, duration_days, restrict_to_discovered)) any = true;
  }
  return any;
}

bool Simulation::issue_fleet_travel_via_jump(Id fleet_id, Id jump_point_id) {
  prune_fleets();
  const auto* fl = find_ptr(state_.fleets, fleet_id);
  if (!fl) return false;
  bool any = false;
  for (Id sid : fl->ship_ids) {
    if (issue_travel_via_jump(sid, jump_point_id)) any = true;
  }
  return any;
}

bool Simulation::issue_fleet_survey_jump_point(Id fleet_id, Id jump_point_id, bool transit_when_done,
                                              bool restrict_to_discovered) {
  prune_fleets();
  const auto* fl = find_ptr(state_.fleets, fleet_id);
  if (!fl) return false;
  bool any = false;
  for (Id sid : fl->ship_ids) {
    if (issue_survey_jump_point(sid, jump_point_id, transit_when_done, restrict_to_discovered)) any = true;
  }
  return any;
}

bool Simulation::issue_fleet_travel_to_system(Id fleet_id, Id target_system_id, bool restrict_to_discovered) {
  prune_fleets();
  const auto* fl = find_ptr(state_.fleets, fleet_id);
  if (!fl) return false;

  if (!find_ptr(state_.systems, target_system_id)) return false;
  if (fl->ship_ids.empty()) return false;

  // Prefer routing once for the whole fleet so every ship takes the same hop sequence.
  // If ships are not co-located (after their queued jumps), fall back to per-ship routing.
  Id leader_id = fl->leader_ship_id;
  const Ship* leader = (leader_id != kInvalidId) ? find_ptr(state_.ships, leader_id) : nullptr;
  if (!leader) {
    leader_id = kInvalidId;
    for (Id sid : fl->ship_ids) {
      if (const auto* sh = find_ptr(state_.ships, sid)) {
        leader_id = sid;
        leader = sh;
        break;
      }
    }
  }

  if (!leader) return false;

  const PredictedNavState leader_nav = predicted_nav_state_after_queued_jumps(state_, leader_id,
                                                                              /*include_queued_jumps=*/true);
  if (leader_nav.system_id == kInvalidId) return false;

  bool colocated = true;
  for (Id sid : fl->ship_ids) {
    const PredictedNavState nav = predicted_nav_state_after_queued_jumps(state_, sid, /*include_queued_jumps=*/true);
    if (nav.system_id != leader_nav.system_id) {
      colocated = false;
      break;
    }
  }

  if (!colocated) {
    bool any = false;
    for (Id sid : fl->ship_ids) {
      if (issue_travel_to_system(sid, target_system_id, restrict_to_discovered)) any = true;
    }
    return any;
  }

  if (leader_nav.system_id == target_system_id) return true; // no-op

  const auto plan = plan_jump_route_cached(leader_nav.system_id, leader_nav.position_mkm,
                                          fl->faction_id, leader->speed_km_s, target_system_id,
                                          restrict_to_discovered);
  if (!plan) return false;

  bool any = false;
  for (Id sid : fl->ship_ids) {
    if (!find_ptr(state_.ships, sid)) continue;
    auto& orders = state_.ship_orders[sid];
    for (Id jid : plan->jump_ids) orders.queue.push_back(TravelViaJump{jid});
    any = true;
  }
  return any;
}

bool Simulation::issue_fleet_attack_ship(Id fleet_id, Id target_ship_id, bool restrict_to_discovered) {
  prune_fleets();
  const auto* fl = find_ptr(state_.fleets, fleet_id);
  if (!fl) return false;
  bool any = false;
  for (Id sid : fl->ship_ids) {
    if (issue_attack_ship(sid, target_ship_id, restrict_to_discovered)) any = true;
  }
  return any;
}

bool Simulation::issue_fleet_escort_ship(Id fleet_id, Id target_ship_id, double follow_distance_mkm,
                                        bool restrict_to_discovered) {
  prune_fleets();
  const auto* fl = find_ptr(state_.fleets, fleet_id);
  if (!fl) return false;
  bool any = false;
  for (Id sid : fl->ship_ids) {
    if (issue_escort_ship(sid, target_ship_id, follow_distance_mkm, restrict_to_discovered)) any = true;
  }
  return any;
}

bool Simulation::issue_fleet_bombard_colony(Id fleet_id, Id colony_id, int duration_days,
                                           bool restrict_to_discovered) {
  prune_fleets();
  const auto* fl = find_ptr(state_.fleets, fleet_id);
  if (!fl) return false;
  bool any = false;
  for (Id sid : fl->ship_ids) {
    if (issue_bombard_colony(sid, colony_id, duration_days, restrict_to_discovered)) any = true;
  }
  return any;
}

bool Simulation::issue_fleet_load_mineral(Id fleet_id, Id colony_id, const std::string& mineral, double tons,
                                          bool restrict_to_discovered) {
  prune_fleets();
  const auto* fl = find_ptr(state_.fleets, fleet_id);
  if (!fl) return false;
  bool any = false;
  for (Id sid : fl->ship_ids) {
    if (issue_load_mineral(sid, colony_id, mineral, tons, restrict_to_discovered)) any = true;
  }
  return any;
}

bool Simulation::issue_fleet_unload_mineral(Id fleet_id, Id colony_id, const std::string& mineral, double tons,
                                            bool restrict_to_discovered) {
  prune_fleets();
  const auto* fl = find_ptr(state_.fleets, fleet_id);
  if (!fl) return false;
  bool any = false;
  for (Id sid : fl->ship_ids) {
    if (issue_unload_mineral(sid, colony_id, mineral, tons, restrict_to_discovered)) any = true;
  }
  return any;
}

bool Simulation::issue_fleet_salvage_wreck(Id fleet_id, Id wreck_id, const std::string& mineral, double tons,
                                           bool restrict_to_discovered) {
  prune_fleets();
  const auto* fl = find_ptr(state_.fleets, fleet_id);
  if (!fl) return false;
  bool any = false;
  for (Id sid : fl->ship_ids) {
    if (issue_salvage_wreck(sid, wreck_id, mineral, tons, restrict_to_discovered)) any = true;
  }
  return any;
}


bool Simulation::issue_fleet_investigate_anomaly(Id fleet_id, Id anomaly_id, bool restrict_to_discovered) {
  prune_fleets();
  const auto* fl = find_ptr(state_.fleets, fleet_id);
  if (!fl) return false;
  bool any = false;
  for (Id sid : fl->ship_ids) {
    if (issue_investigate_anomaly(sid, anomaly_id, restrict_to_discovered)) any = true;
  }
  return any;
}

bool Simulation::issue_fleet_transfer_cargo_to_ship(Id fleet_id, Id target_ship_id, const std::string& mineral,
                                                    double tons, bool restrict_to_discovered) {
  prune_fleets();
  const auto* fl = find_ptr(state_.fleets, fleet_id);
  if (!fl) return false;
  bool any = false;
  for (Id sid : fl->ship_ids) {
    if (issue_transfer_cargo_to_ship(sid, target_ship_id, mineral, tons, restrict_to_discovered)) any = true;
  }
  return any;
}

bool Simulation::issue_fleet_scrap_ship(Id fleet_id, Id colony_id, bool restrict_to_discovered) {
  prune_fleets();
  const auto* fl = find_ptr(state_.fleets, fleet_id);
  if (!fl) return false;
  bool any = false;
  for (Id sid : fl->ship_ids) {
    if (issue_scrap_ship(sid, colony_id, restrict_to_discovered)) any = true;
  }
  return any;
}

bool Simulation::issue_wait_days(Id ship_id, int days) {
  if (days <= 0) return false;
  if (!find_ptr(state_.ships, ship_id)) return false;
  auto& orders = state_.ship_orders[ship_id];
  orders.queue.push_back(WaitDays{days});
  return true;
}

bool Simulation::issue_move_to_point(Id ship_id, Vec2 target_mkm) {
  auto* ship = find_ptr(state_.ships, ship_id);
  if (!ship) return false;
  auto& orders = state_.ship_orders[ship_id];
  orders.queue.push_back(MoveToPoint{target_mkm});
  return true;
}

bool Simulation::issue_move_to_body(Id ship_id, Id body_id, bool restrict_to_discovered) {
  auto* ship = find_ptr(state_.ships, ship_id);
  if (!ship) return false;
  const auto* body = find_ptr(state_.bodies, body_id);
  if (!body) return false;

  const Id target_system_id = body->system_id;
  if (target_system_id == kInvalidId) return false;
  if (!find_ptr(state_.systems, target_system_id)) return false;

  if (!issue_travel_to_system(ship_id, target_system_id, restrict_to_discovered, body->position_mkm)) return false;

  auto& orders = state_.ship_orders[ship_id];
  orders.queue.push_back(MoveToBody{body_id});
  return true;
}

bool Simulation::issue_colonize_body(Id ship_id, Id body_id, const std::string& colony_name,
                                    bool restrict_to_discovered) {
  const auto* body = find_ptr(state_.bodies, body_id);
  if (!body) return false;

  // Route across the jump network if needed.
  if (!issue_travel_to_system(ship_id, body->system_id, restrict_to_discovered, body->position_mkm)) return false;

  auto& q = state_.ship_orders[ship_id].queue;
  ColonizeBody ord;
  ord.body_id = body_id;
  ord.colony_name = colony_name;
  q.push_back(std::move(ord));
  return true;
}

bool Simulation::issue_orbit_body(Id ship_id, Id body_id, int duration_days, bool restrict_to_discovered) {
  auto* ship = find_ptr(state_.ships, ship_id);
  if (!ship) return false;
  const auto* body = find_ptr(state_.bodies, body_id);
  if (!body) return false;
  
  const Id target_system_id = body->system_id;
  if (target_system_id == kInvalidId) return false;
  if (!find_ptr(state_.systems, target_system_id)) return false;

  if (!issue_travel_to_system(ship_id, target_system_id, restrict_to_discovered, body->position_mkm)) return false;

  auto& orders = state_.ship_orders[ship_id];
  orders.queue.push_back(OrbitBody{body_id, duration_days});
  return true;
}

bool Simulation::issue_travel_via_jump(Id ship_id, Id jump_point_id) {
  auto* ship = find_ptr(state_.ships, ship_id);
  if (!ship) return false;
  if (!find_ptr(state_.jump_points, jump_point_id)) return false;
  auto& orders = state_.ship_orders[ship_id];
  orders.queue.push_back(TravelViaJump{jump_point_id});
  return true;
}


bool Simulation::issue_survey_jump_point(Id ship_id, Id jump_point_id, bool transit_when_done,
                                        bool restrict_to_discovered) {
  auto* ship = find_ptr(state_.ships, ship_id);
  if (!ship) return false;

  const auto* jp = find_ptr(state_.jump_points, jump_point_id);
  if (!jp) return false;
  if (jp->system_id == kInvalidId) return false;
  if (!find_ptr(state_.systems, jp->system_id)) return false;

  // Travel to the jump point's system if needed (goal-aware).
  if (!issue_travel_to_system(ship_id, jp->system_id, restrict_to_discovered, jp->position_mkm)) return false;

  SurveyJumpPoint ord;
  ord.jump_point_id = jump_point_id;
  ord.transit_when_done = transit_when_done;

  auto& orders = state_.ship_orders[ship_id];
  orders.queue.push_back(std::move(ord));
  return true;
}

bool Simulation::issue_travel_to_system(Id ship_id, Id target_system_id, bool restrict_to_discovered,
                                     std::optional<Vec2> goal_pos_mkm) {
  auto* ship = find_ptr(state_.ships, ship_id);
  if (!ship) return false;
  if (!find_ptr(state_.systems, target_system_id)) return false;

  const PredictedNavState nav = predicted_nav_state_after_queued_jumps(state_, ship_id, /*include_queued_jumps=*/true);
  if (nav.system_id == kInvalidId) return false;
  if (nav.system_id == target_system_id) return true; // no-op

  const auto plan = plan_jump_route_cached(nav.system_id, nav.position_mkm, ship->faction_id, ship->speed_km_s,
                                          target_system_id, restrict_to_discovered, goal_pos_mkm);
  if (!plan) return false;

  auto& orders = state_.ship_orders[ship_id];
  for (Id jid : plan->jump_ids) orders.queue.push_back(TravelViaJump{jid});
  return true;
}

bool Simulation::issue_attack_ship(Id attacker_ship_id, Id target_ship_id, bool restrict_to_discovered) {
  if (attacker_ship_id == target_ship_id) return false;
  auto* attacker = find_ptr(state_.ships, attacker_ship_id);
  if (!attacker) return false;
  const auto* target = find_ptr(state_.ships, target_ship_id);
  if (!target) return false;
  if (target->faction_id == attacker->faction_id) return false;

  // If there is an active treaty between the factions, require the player/AI to
  // explicitly cancel the treaty / declare hostilities first. This prevents
  // ceasefires and other agreements from being immediately broken by queued
  // attack orders.
  TreatyType tt = TreatyType::Ceasefire;
  if (strongest_active_treaty_between(state_, attacker->faction_id, target->faction_id, &tt)) {
    std::ostringstream oss;
    oss << "Attack order blocked by active " << treaty_type_display_name(tt)
        << " between factions.";
    EventContext ctx;
    ctx.faction_id = attacker->faction_id;
    ctx.faction_id2 = target->faction_id;
    ctx.ship_id = attacker_ship_id;
    ctx.system_id = attacker->system_id;
    this->push_event(EventLevel::Warn, EventCategory::Diplomacy, oss.str(), ctx);
    return false;
  }

  const bool detected = is_ship_detected_by_faction(attacker->faction_id, target_ship_id);

  AttackShip ord;
  ord.target_ship_id = target_ship_id;

  Id target_system_id = kInvalidId;

  if (detected) {
    ord.has_last_known = true;
    ord.last_known_position_mkm = target->position_mkm;
    target_system_id = target->system_id;
  } else {
    const auto* fac = find_ptr(state_.factions, attacker->faction_id);
    if (!fac) return false;
    const auto it = fac->ship_contacts.find(target_ship_id);
    if (it == fac->ship_contacts.end()) return false;
    ord.has_last_known = true;

    // If we have a 2-point contact track, extrapolate a better last-known
    // position to pursue under fog-of-war.
    const int now = static_cast<int>(state_.date.days_since_epoch());
    const auto pred = predict_contact_position(it->second, now, cfg_.contact_prediction_max_days);
    ord.last_known_position_mkm = pred.predicted_position_mkm;

    target_system_id = it->second.system_id;
  }

  if (target_system_id == kInvalidId) return false;
  if (!find_ptr(state_.systems, target_system_id)) return false;

  if (!issue_travel_to_system(attacker_ship_id, target_system_id, restrict_to_discovered,
                            ord.last_known_position_mkm)) return false;

  auto& orders = state_.ship_orders[attacker_ship_id];
  orders.queue.push_back(ord);
  return true;
}

bool Simulation::issue_escort_ship(Id escort_ship_id, Id target_ship_id, double follow_distance_mkm,
                                  bool restrict_to_discovered) {
  if (escort_ship_id == target_ship_id) return false;
  auto* escort = find_ptr(state_.ships, escort_ship_id);
  if (!escort) return false;
  const auto* target = find_ptr(state_.ships, target_ship_id);
  if (!target) return false;
  if (!are_factions_mutual_friendly(escort->faction_id, target->faction_id)) return false;
  if (!std::isfinite(follow_distance_mkm) || follow_distance_mkm < 0.0) return false;
  if (follow_distance_mkm <= 0.0) follow_distance_mkm = std::max(0.0, cfg_.docking_range_mkm);

  EscortShip ord;
  ord.target_ship_id = target_ship_id;
  ord.follow_distance_mkm = follow_distance_mkm;
  ord.restrict_to_discovered = restrict_to_discovered;

  auto& orders = state_.ship_orders[escort_ship_id];
  orders.queue.push_back(std::move(ord));
  return true;
}

bool Simulation::issue_load_mineral(Id ship_id, Id colony_id, const std::string& mineral, double tons,
                                    bool restrict_to_discovered) {
  auto* ship = find_ptr(state_.ships, ship_id);
  if (!ship) return false;
  auto* colony = find_ptr(state_.colonies, colony_id);
  if (!colony) return false;
  if (!are_factions_trade_partners(ship->faction_id, colony->faction_id)) return false;
  const auto* body = find_ptr(state_.bodies, colony->body_id);
  if (!body) return false;
  if (body->system_id == kInvalidId) return false;
  if (!find_ptr(state_.systems, body->system_id)) return false;
  if (tons < 0.0) return false;

  if (!issue_travel_to_system(ship_id, body->system_id, restrict_to_discovered, body->position_mkm)) return false;

  auto& orders = state_.ship_orders[ship_id];
  orders.queue.push_back(LoadMineral{colony_id, mineral, tons});
  return true;
}

bool Simulation::issue_unload_mineral(Id ship_id, Id colony_id, const std::string& mineral, double tons,
                                      bool restrict_to_discovered) {
  auto* ship = find_ptr(state_.ships, ship_id);
  if (!ship) return false;
  auto* colony = find_ptr(state_.colonies, colony_id);
  if (!colony) return false;
  if (!are_factions_trade_partners(ship->faction_id, colony->faction_id)) return false;
  const auto* body = find_ptr(state_.bodies, colony->body_id);
  if (!body) return false;
  if (body->system_id == kInvalidId) return false;
  if (!find_ptr(state_.systems, body->system_id)) return false;
  if (tons < 0.0) return false;

  if (!issue_travel_to_system(ship_id, body->system_id, restrict_to_discovered, body->position_mkm)) return false;

  auto& orders = state_.ship_orders[ship_id];
  orders.queue.push_back(UnloadMineral{colony_id, mineral, tons});
  return true;
}

bool Simulation::issue_salvage_wreck(Id ship_id, Id wreck_id, const std::string& mineral, double tons,
                                    bool restrict_to_discovered) {
  auto* ship = find_ptr(state_.ships, ship_id);
  if (!ship) return false;
  const auto* wreck = find_ptr(state_.wrecks, wreck_id);
  if (!wreck) return false;
  if (tons < 0.0) return false;

  const Id sys_id = wreck->system_id;
  if (sys_id == kInvalidId) return false;
  if (!find_ptr(state_.systems, sys_id)) return false;

  if (!issue_travel_to_system(ship_id, sys_id, restrict_to_discovered, wreck->position_mkm)) return false;

  auto& orders = state_.ship_orders[ship_id];
  orders.queue.push_back(SalvageWreck{wreck_id, mineral, tons});
  return true;
}



bool Simulation::issue_investigate_anomaly(Id ship_id, Id anomaly_id, bool restrict_to_discovered) {
  auto* ship = find_ptr(state_.ships, ship_id);
  if (!ship) return false;
  const auto* an = find_ptr(state_.anomalies, anomaly_id);
  if (!an) return false;
  if (an->resolved) return false;

  const Id sys_id = an->system_id;
  if (sys_id == kInvalidId) return false;
  if (!find_ptr(state_.systems, sys_id)) return false;

  if (!issue_travel_to_system(ship_id, sys_id, restrict_to_discovered, an->position_mkm)) return false;

  InvestigateAnomaly ord;
  ord.anomaly_id = anomaly_id;
  ord.duration_days = std::max(0, an->investigation_days);

  auto& orders = state_.ship_orders[ship_id];
  orders.queue.push_back(std::move(ord));
  return true;
}

bool Simulation::issue_mine_body(Id ship_id, Id body_id, const std::string& mineral,
                                bool stop_when_cargo_full, bool restrict_to_discovered) {
  auto* ship = find_ptr(state_.ships, ship_id);
  if (!ship) return false;

  const auto* body = find_ptr(state_.bodies, body_id);
  if (!body) return false;

  // Require some mining capacity.
  const auto* d = find_design(ship->design_id);
  const double mine_rate = d ? std::max(0.0, d->mining_tons_per_day) : 0.0;
  if (mine_rate <= 1e-9) return false;

  // Travel to the target system if needed.
  if (body->system_id != ship->system_id) {
    // Use goal-aware routing to prefer entry jump points closer to the body.
    if (!issue_travel_to_system(ship_id, body->system_id, restrict_to_discovered, body->position_mkm)) return false;
  }

  ShipOrders& so = state_.ship_orders[ship_id];
  MineBody mb;
  mb.body_id = body_id;
  mb.mineral = mineral;
  mb.stop_when_cargo_full = stop_when_cargo_full;
  so.queue.push_back(mb);
  return true;
}

bool Simulation::issue_load_troops(Id ship_id, Id colony_id, double strength, bool restrict_to_discovered) {
  auto* ship = find_ptr(state_.ships, ship_id);
  if (!ship) return false;
  auto* colony = find_ptr(state_.colonies, colony_id);
  if (!colony) return false;
  if (colony->faction_id != ship->faction_id) return false;
  const auto* body = find_ptr(state_.bodies, colony->body_id);
  if (!body) return false;
  if (body->system_id == kInvalidId) return false;
  if (!find_ptr(state_.systems, body->system_id)) return false;
  if (strength < 0.0) return false;

  if (!issue_travel_to_system(ship_id, body->system_id, restrict_to_discovered, body->position_mkm)) return false;

  auto& orders = state_.ship_orders[ship_id];
  orders.queue.push_back(LoadTroops{colony_id, strength});
  return true;
}

bool Simulation::issue_unload_troops(Id ship_id, Id colony_id, double strength, bool restrict_to_discovered) {
  auto* ship = find_ptr(state_.ships, ship_id);
  if (!ship) return false;
  auto* colony = find_ptr(state_.colonies, colony_id);
  if (!colony) return false;
  if (colony->faction_id != ship->faction_id) return false;
  const auto* body = find_ptr(state_.bodies, colony->body_id);
  if (!body) return false;
  if (body->system_id == kInvalidId) return false;
  if (!find_ptr(state_.systems, body->system_id)) return false;
  if (strength < 0.0) return false;

  if (!issue_travel_to_system(ship_id, body->system_id, restrict_to_discovered, body->position_mkm)) return false;

  auto& orders = state_.ship_orders[ship_id];
  orders.queue.push_back(UnloadTroops{colony_id, strength});
  return true;
}

bool Simulation::issue_load_colonists(Id ship_id, Id colony_id, double millions, bool restrict_to_discovered) {
  auto* ship = find_ptr(state_.ships, ship_id);
  if (!ship) return false;
  auto* colony = find_ptr(state_.colonies, colony_id);
  if (!colony) return false;
  if (colony->faction_id != ship->faction_id) return false;
  const auto* d = find_design(ship->design_id);
  if (!d) return false;
  if (d->colony_capacity_millions <= 0.0) return false;
  if (millions < 0.0) return false;

  const auto* body = find_ptr(state_.bodies, colony->body_id);
  if (!body) return false;
  if (body->system_id == kInvalidId) return false;
  if (!find_ptr(state_.systems, body->system_id)) return false;

  if (!issue_travel_to_system(ship_id, body->system_id, restrict_to_discovered, body->position_mkm)) return false;

  auto& orders = state_.ship_orders[ship_id];
  orders.queue.push_back(LoadColonists{colony_id, millions});
  return true;
}

bool Simulation::issue_unload_colonists(Id ship_id, Id colony_id, double millions, bool restrict_to_discovered) {
  auto* ship = find_ptr(state_.ships, ship_id);
  if (!ship) return false;
  auto* colony = find_ptr(state_.colonies, colony_id);
  if (!colony) return false;
  if (colony->faction_id != ship->faction_id) return false;
  const auto* d = find_design(ship->design_id);
  if (!d) return false;
  if (d->colony_capacity_millions <= 0.0) return false;
  if (millions < 0.0) return false;

  const auto* body = find_ptr(state_.bodies, colony->body_id);
  if (!body) return false;
  if (body->system_id == kInvalidId) return false;
  if (!find_ptr(state_.systems, body->system_id)) return false;

  if (!issue_travel_to_system(ship_id, body->system_id, restrict_to_discovered, body->position_mkm)) return false;

  auto& orders = state_.ship_orders[ship_id];
  orders.queue.push_back(UnloadColonists{colony_id, millions});
  return true;
}

bool Simulation::issue_invade_colony(Id ship_id, Id colony_id, bool restrict_to_discovered) {
  auto* ship = find_ptr(state_.ships, ship_id);
  if (!ship) return false;
  auto* colony = find_ptr(state_.colonies, colony_id);
  if (!colony) return false;
  if (colony->faction_id == ship->faction_id) return false;

  TreatyType tt = TreatyType::Ceasefire;
  if (strongest_active_treaty_between(state_, ship->faction_id, colony->faction_id, &tt)) {
    std::ostringstream oss;
    oss << "Invasion order blocked by active " << treaty_type_display_name(tt)
        << " between factions.";
    EventContext ctx;
    ctx.faction_id = ship->faction_id;
    ctx.faction_id2 = colony->faction_id;
    ctx.ship_id = ship_id;
    this->push_event(EventLevel::Warn, EventCategory::Diplomacy, oss.str(), ctx);
    return false;
  }

  const auto* body = find_ptr(state_.bodies, colony->body_id);
  if (!body) return false;
  if (body->system_id == kInvalidId) return false;
  if (!find_ptr(state_.systems, body->system_id)) return false;

  if (!issue_travel_to_system(ship_id, body->system_id, restrict_to_discovered, body->position_mkm)) return false;

  auto& orders = state_.ship_orders[ship_id];
  orders.queue.push_back(InvadeColony{colony_id});
  return true;
}

bool Simulation::issue_bombard_colony(Id ship_id, Id colony_id, int duration_days, bool restrict_to_discovered) {
  auto* ship = find_ptr(state_.ships, ship_id);
  if (!ship) return false;
  auto* colony = find_ptr(state_.colonies, colony_id);
  if (!colony) return false;
  if (colony->faction_id == ship->faction_id) return false;

  TreatyType tt = TreatyType::Ceasefire;
  if (strongest_active_treaty_between(state_, ship->faction_id, colony->faction_id, &tt)) {
    std::ostringstream oss;
    oss << "Bombardment order blocked by active " << treaty_type_display_name(tt)
        << " between factions.";
    EventContext ctx;
    ctx.faction_id = ship->faction_id;
    ctx.faction_id2 = colony->faction_id;
    ctx.ship_id = ship_id;
    this->push_event(EventLevel::Warn, EventCategory::Diplomacy, oss.str(), ctx);
    return false;
  }

  if (duration_days < -1) return false;
  if (duration_days == 0) return false;

  const auto* d = find_design(ship->design_id);
  if (!d) return false;
  if (d->weapon_damage <= 0.0 || d->weapon_range_mkm <= 0.0) return false;

  const auto* body = find_ptr(state_.bodies, colony->body_id);
  if (!body) return false;
  if (body->system_id == kInvalidId) return false;
  if (!find_ptr(state_.systems, body->system_id)) return false;

  if (!issue_travel_to_system(ship_id, body->system_id, restrict_to_discovered, body->position_mkm)) return false;

  auto& orders = state_.ship_orders[ship_id];
  orders.queue.push_back(BombardColony{colony_id, duration_days});
  return true;
}

bool Simulation::enqueue_troop_training(Id colony_id, double strength) {
  auto* colony = find_ptr(state_.colonies, colony_id);
  if (!colony) return false;
  if (strength <= 0.0) return false;
  colony->troop_training_queue += strength;
  return true;
}

bool Simulation::clear_troop_training_queue(Id colony_id) {
  auto* colony = find_ptr(state_.colonies, colony_id);
  if (!colony) return false;
  colony->troop_training_queue = 0.0;
  colony->troop_training_auto_queued = 0.0;
  return true;
}

bool Simulation::set_terraforming_target(Id body_id, double target_temp_k, double target_atm) {
  auto* body = find_ptr(state_.bodies, body_id);
  if (!body) return false;
  if (target_temp_k <= 0.0 && target_atm <= 0.0) return false;
  body->terraforming_target_temp_k = std::max(0.0, target_temp_k);
  body->terraforming_target_atm = std::max(0.0, target_atm);
  body->terraforming_complete = false;
  return true;
}

bool Simulation::clear_terraforming_target(Id body_id) {
  auto* body = find_ptr(state_.bodies, body_id);
  if (!body) return false;
  body->terraforming_target_temp_k = 0.0;
  body->terraforming_target_atm = 0.0;
  body->terraforming_complete = false;
  return true;
}

double Simulation::terraforming_points_per_day(const Colony& c) const {
  double total = 0.0;
  for (const auto& [inst_id, count] : c.installations) {
    if (count <= 0) continue;
    auto it = content_.installations.find(inst_id);
    if (it == content_.installations.end()) continue;
    const double p = it->second.terraforming_points_per_day;
    if (p > 0.0) total += p * static_cast<double>(count);
  }
  if (const auto* fac = find_ptr(state_.factions, c.faction_id)) {
    const auto m = compute_faction_economy_multipliers(content_, *fac);
    total *= std::max(0.0, m.terraforming);
  }
  return std::max(0.0, total);
}

double Simulation::troop_training_points_per_day(const Colony& c) const {
  double total = 0.0;
  for (const auto& [inst_id, count] : c.installations) {
    if (count <= 0) continue;
    auto it = content_.installations.find(inst_id);
    if (it == content_.installations.end()) continue;
    const double p = it->second.troop_training_points_per_day;
    if (p > 0.0) total += p * static_cast<double>(count);
  }
  if (const auto* fac = find_ptr(state_.factions, c.faction_id)) {
    const auto m = compute_faction_economy_multipliers(content_, *fac);
    total *= std::max(0.0, m.troop_training);
  }
  return std::max(0.0, total);
}

double Simulation::crew_training_points_per_day(const Colony& c) const {
  double total = 0.0;
  for (const auto& [inst_id, count] : c.installations) {
    if (count <= 0) continue;
    auto it = content_.installations.find(inst_id);
    if (it == content_.installations.end()) continue;
    const double p = it->second.crew_training_points_per_day;
    if (p > 0.0) total += p * static_cast<double>(count);
  }
  // Crew training currently uses the same faction economy multiplier bucket as troop training.
  if (const auto* fac = find_ptr(state_.factions, c.faction_id)) {
    const auto m = compute_faction_economy_multipliers(content_, *fac);
    total *= std::max(0.0, m.troop_training);
  }
  total *= std::max(0.0, cfg_.crew_training_points_multiplier);
  return std::max(0.0, total);
}

double Simulation::fortification_points(const Colony& c) const {
  double total = 0.0;
  for (const auto& [inst_id, count] : c.installations) {
    if (count <= 0) continue;
    auto it = content_.installations.find(inst_id);
    if (it == content_.installations.end()) continue;
    const double p = it->second.fortification_points;
    if (p > 0.0) total += p * static_cast<double>(count);
  }
  return total;
}

double Simulation::crew_grade_bonus_for_points(double grade_points) const {
  if (!cfg_.enable_crew_experience) return 0.0;
  if (!std::isfinite(grade_points)) grade_points = cfg_.crew_initial_grade_points;
  const double cap = std::max(0.0, cfg_.crew_grade_points_cap);
  grade_points = std::clamp(grade_points, 0.0, cap > 0.0 ? cap : grade_points);
  // Aurora-style grade points mapping.
  // bonus = (sqrt(points) - 10) / 100
  // points=100 -> 0; points=400 -> +10%; points=0 -> -10%.
  const double bonus = (std::sqrt(std::max(0.0, grade_points)) - 10.0) / 100.0;
  // Safety clamp to a sane range even if mods set extreme caps.
  return std::clamp(bonus, -0.25, 0.75);
}

double Simulation::crew_grade_bonus(const Ship& ship) const {
  return crew_grade_bonus_for_points(ship.crew_grade_points);
}

bool Simulation::issue_transfer_cargo_to_ship(Id ship_id, Id target_ship_id, const std::string& mineral, double tons,
                                              bool restrict_to_discovered) {
  auto* ship = find_ptr(state_.ships, ship_id);
  if (!ship) return false;
  auto* target = find_ptr(state_.ships, target_ship_id);
  if (!target) return false;
  
  if (ship->faction_id != target->faction_id) return false;
  if (tons < 0.0) return false;

  if (!issue_travel_to_system(ship_id, target->system_id, restrict_to_discovered, target->position_mkm)) return false;

  auto& orders = state_.ship_orders[ship_id];
  orders.queue.push_back(TransferCargoToShip{target_ship_id, mineral, tons});
  return true;
}

bool Simulation::issue_transfer_fuel_to_ship(Id ship_id, Id target_ship_id, double tons,
                                             bool restrict_to_discovered) {
  auto* ship = find_ptr(state_.ships, ship_id);
  if (!ship) return false;
  auto* target = find_ptr(state_.ships, target_ship_id);
  if (!target) return false;

  if (ship->faction_id != target->faction_id) return false;
  if (tons < 0.0) return false;

  // Both ships must be capable of storing fuel.
  const auto* src_design = find_design(ship->design_id);
  const auto* tgt_design = find_design(target->design_id);
  if (!src_design || !tgt_design) return false;
  if (src_design->fuel_capacity_tons <= 0.0) return false;
  if (tgt_design->fuel_capacity_tons <= 0.0) return false;

  if (!issue_travel_to_system(ship_id, target->system_id, restrict_to_discovered, target->position_mkm)) return false;

  auto& orders = state_.ship_orders[ship_id];
  orders.queue.push_back(TransferFuelToShip{target_ship_id, tons});
  return true;
}

bool Simulation::issue_transfer_troops_to_ship(Id ship_id, Id target_ship_id, double strength,
                                               bool restrict_to_discovered) {
  auto* ship = find_ptr(state_.ships, ship_id);
  if (!ship) return false;
  auto* target = find_ptr(state_.ships, target_ship_id);
  if (!target) return false;

  if (ship->faction_id != target->faction_id) return false;
  if (strength < 0.0) return false;

  // Both ships must be capable of carrying troops.
  const auto* src_design = find_design(ship->design_id);
  const auto* tgt_design = find_design(target->design_id);
  if (!src_design || !tgt_design) return false;
  if (src_design->troop_capacity <= 0.0) return false;
  if (tgt_design->troop_capacity <= 0.0) return false;

  if (!issue_travel_to_system(ship_id, target->system_id, restrict_to_discovered, target->position_mkm)) return false;

  auto& orders = state_.ship_orders[ship_id];
  orders.queue.push_back(TransferTroopsToShip{target_ship_id, strength});
  return true;
}

bool Simulation::issue_scrap_ship(Id ship_id, Id colony_id, bool restrict_to_discovered) {
  auto* ship = find_ptr(state_.ships, ship_id);
  if (!ship) return false;
  auto* colony = find_ptr(state_.colonies, colony_id);
  if (!colony) return false;
  if (colony->faction_id != ship->faction_id) return false;

  const auto* body = find_ptr(state_.bodies, colony->body_id);
  if (!body) return false;

  if (!issue_travel_to_system(ship_id, body->system_id, restrict_to_discovered, body->position_mkm)) return false;

  auto& orders = state_.ship_orders[ship_id];
  orders.queue.push_back(ScrapShip{colony_id});
  return true;
}

bool Simulation::enqueue_build(Id colony_id, const std::string& design_id) {
  auto* colony = find_ptr(state_.colonies, colony_id);
  if (!colony) return false;
  const auto it_yard = colony->installations.find("shipyard");
  if (it_yard == colony->installations.end() || it_yard->second <= 0) return false;
  const auto* d = find_design(design_id);
  if (!d) return false;
  if (!is_design_buildable_for_faction(colony->faction_id, design_id)) return false;
  BuildOrder bo;
  bo.design_id = design_id;
  bo.tons_remaining = std::max(1.0, d->mass_tons);
  colony->shipyard_queue.push_back(bo);
  return true;
}

double Simulation::estimate_refit_tons(Id ship_id, const std::string& target_design_id) const {
  const auto* ship = find_ptr(state_.ships, ship_id);
  if (!ship) return 0.0;

  const auto* target = find_design(target_design_id);
  if (!target) return 0.0;

  const double mult = std::max(0.0, cfg_.ship_refit_tons_multiplier);
  return std::max(1.0, target->mass_tons * mult);
}

bool Simulation::enqueue_refit(Id colony_id, Id ship_id, const std::string& target_design_id, std::string* error) {
  auto fail = [&](const std::string& msg) {
    if (error) *error = msg;
    return false;
  };

  auto* colony = find_ptr(state_.colonies, colony_id);
  if (!colony) return fail("Colony not found");

  auto* ship = find_ptr(state_.ships, ship_id);
  if (!ship) return fail("Ship not found");

  if (ship->faction_id != colony->faction_id) return fail("Ship does not belong to the colony faction");

  const auto it_yard = colony->installations.find("shipyard");
  const int yards = (it_yard != colony->installations.end()) ? it_yard->second : 0;
  if (yards <= 0) return fail("Colony has no shipyard");

  const auto* target = find_design(target_design_id);
  if (!target) return fail("Unknown target design: " + target_design_id);
  if (!is_design_buildable_for_faction(colony->faction_id, target_design_id)) return fail("Target design is not unlocked");

  // Refit requires the ship to be docked at the colony at the time of queuing.
  if (!is_ship_docked_at_colony(ship_id, colony_id)) return fail("Ship is not docked at the colony");

  // Keep the prototype simple: refit ships must be detached from fleets.
  if (fleet_for_ship(ship_id) != kInvalidId) return fail("Ship is assigned to a fleet (detach before refit)");

  // Prevent duplicate queued refits for the same ship.
  for (const auto& [_, c] : state_.colonies) {
    for (const auto& bo : c.shipyard_queue) {
      if (bo.refit_ship_id == ship_id) return fail("Ship already has a pending refit order");
    }
  }

  BuildOrder bo;
  bo.design_id = target_design_id;
  bo.refit_ship_id = ship_id;
  bo.tons_remaining = estimate_refit_tons(ship_id, target_design_id);
  colony->shipyard_queue.push_back(bo);

  // Log a helpful event for the player.
  {
    EventContext ctx;
    ctx.faction_id = colony->faction_id;
    ctx.system_id = ship->system_id;
    ctx.ship_id = ship->id;
    ctx.colony_id = colony->id;

    std::string msg = "Shipyard refit queued: " + ship->name + " -> " + target->name + " at " + colony->name;
    push_event(EventLevel::Info, EventCategory::Shipyard, std::move(msg), ctx);
  }

  return true;
}

bool Simulation::enqueue_installation_build(Id colony_id, const std::string& installation_id, int quantity) {
  auto* colony = find_ptr(state_.colonies, colony_id);
  if (!colony) return false;
  if (quantity <= 0) return false;
  if (content_.installations.find(installation_id) == content_.installations.end()) return false;
  if (!is_installation_buildable_for_faction(colony->faction_id, installation_id)) return false;

  InstallationBuildOrder o;
  o.installation_id = installation_id;
  o.quantity_remaining = quantity;
  colony->construction_queue.push_back(o);
  return true;
}


} // namespace nebula4x
