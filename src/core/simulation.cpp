#include "nebula4x/core/simulation.h"

#include <cmath>
#include <stdexcept>

#include "nebula4x/util/log.h"
#include "nebula4x/core/scenario.h"

namespace nebula4x {
namespace {

constexpr double kTwoPi = 6.283185307179586476925286766559;

double mkm_per_day_from_speed(double speed_km_s, double seconds_per_day) {
  const double km_per_day = speed_km_s * seconds_per_day;
  return km_per_day / 1.0e6; // million km
}

} // namespace

Simulation::Simulation(ContentDB content, SimConfig cfg) : content_(std::move(content)), cfg_(cfg) {
  new_game();
}

void Simulation::new_game() {
  state_ = make_sol_scenario();

  // Apply design stats to ships.
  for (auto& [_, ship] : state_.ships) {
    auto it = content_.designs.find(ship.design_id);
    if (it != content_.designs.end()) {
      ship.speed_km_s = it->second.speed_km_s;
    }
  }

  recompute_body_positions();
}

void Simulation::load_game(GameState loaded) {
  state_ = std::move(loaded);

  // Re-derive ship stats in case content changed.
  for (auto& [_, ship] : state_.ships) {
    auto it = content_.designs.find(ship.design_id);
    if (it != content_.designs.end()) {
      ship.speed_km_s = it->second.speed_km_s;
    }
  }

  recompute_body_positions();
}

void Simulation::advance_days(int days) {
  if (days <= 0) return;
  for (int i = 0; i < days; ++i) tick_one_day();
}

bool Simulation::issue_move_to_point(Id ship_id, Vec2 target_mkm) {
  auto* ship = find_ptr(state_.ships, ship_id);
  if (!ship) return false;
  auto& orders = state_.ship_orders[ship_id];
  orders.queue.push_back(MoveToPoint{target_mkm});
  return true;
}

bool Simulation::issue_move_to_body(Id ship_id, Id body_id) {
  auto* ship = find_ptr(state_.ships, ship_id);
  if (!ship) return false;
  if (!find_ptr(state_.bodies, body_id)) return false;
  auto& orders = state_.ship_orders[ship_id];
  orders.queue.push_back(MoveToBody{body_id});
  return true;
}

bool Simulation::enqueue_build(Id colony_id, const std::string& design_id) {
  auto* colony = find_ptr(state_.colonies, colony_id);
  if (!colony) return false;
  auto it = content_.designs.find(design_id);
  if (it == content_.designs.end()) return false;
  BuildOrder bo;
  bo.design_id = design_id;
  bo.tons_remaining = std::max(1.0, it->second.mass_tons);
  colony->shipyard_queue.push_back(bo);
  return true;
}

void Simulation::recompute_body_positions() {
  const double t = static_cast<double>(state_.date.days_since_epoch());
  for (auto& [_, b] : state_.bodies) {
    if (b.orbit_radius_mkm <= 1e-9) {
      b.position_mkm = {0.0, 0.0};
      continue;
    }
    const double period = std::max(1.0, b.orbit_period_days);
    const double theta = b.orbit_phase_radians + kTwoPi * (t / period);
    b.position_mkm = {b.orbit_radius_mkm * std::cos(theta), b.orbit_radius_mkm * std::sin(theta)};
  }
}

void Simulation::tick_one_day() {
  state_.date = state_.date.add_days(1);
  recompute_body_positions();
  tick_colonies();
  tick_shipyards();
  tick_ships();
}

void Simulation::tick_colonies() {
  for (auto& [_, colony] : state_.colonies) {
    for (const auto& [inst_id, count] : colony.installations) {
      auto dit = content_.installations.find(inst_id);
      if (dit == content_.installations.end()) continue;
      for (const auto& [mineral, per_day] : dit->second.produces_per_day) {
        colony.minerals[mineral] += per_day * static_cast<double>(count);
      }
    }
  }
}

void Simulation::tick_shipyards() {
  const auto it_def = content_.installations.find("shipyard");
  const double base_rate = (it_def == content_.installations.end()) ? 0.0 : it_def->second.build_rate_tons_per_day;
  if (base_rate <= 0.0) return;

  for (auto& [_, colony] : state_.colonies) {
    const int yards = colony.installations["shipyard"];
    if (yards <= 0) continue;
    const double rate = base_rate * static_cast<double>(yards);

    while (!colony.shipyard_queue.empty()) {
      auto& bo = colony.shipyard_queue.front();
      bo.tons_remaining -= rate;
      if (bo.tons_remaining > 0.0) break;

      // Build complete: spawn ship at colony body position.
      const auto design_it = content_.designs.find(bo.design_id);
      if (design_it == content_.designs.end()) {
            nebula4x::log::warn(std::string("Unknown design in build queue: ") + bo.design_id);
      } else {
        const auto* body = find_ptr(state_.bodies, colony.body_id);
        if (!body) {
            nebula4x::log::warn("Colony " + std::to_string(colony.id) + " has missing body " + std::to_string(colony.body_id));
        } else {
          const auto* sys = find_ptr(state_.systems, body->system_id);
          if (!sys) {
            nebula4x::log::warn("Body " + std::to_string(body->id) + " has missing system " + std::to_string(body->system_id));
          } else {
            Ship sh;
            sh.id = allocate_id(state_);
            sh.faction_id = colony.faction_id;
            sh.system_id = body->system_id;
            sh.design_id = bo.design_id;
            sh.speed_km_s = design_it->second.speed_km_s;
            sh.position_mkm = body->position_mkm;

            // Simple name numbering.
            sh.name = design_it->second.name + " #" + std::to_string(sh.id);

            state_.ships[sh.id] = sh;
            state_.ship_orders[sh.id] = ShipOrders{};
            state_.systems[sh.system_id].ships.push_back(sh.id);

            nebula4x::log::info("Built ship " + sh.name + " (" + sh.design_id + ") at " + colony.name);
          }
        }
      }

      colony.shipyard_queue.erase(colony.shipyard_queue.begin());
    }
  }
}

void Simulation::tick_ships() {
  for (auto& [ship_id, ship] : state_.ships) {
    auto it = state_.ship_orders.find(ship_id);
    if (it == state_.ship_orders.end()) continue;
    auto& q = it->second.queue;
    if (q.empty()) continue;

    // Determine target.
    Vec2 target = ship.position_mkm;
    if (std::holds_alternative<MoveToPoint>(q.front())) {
      target = std::get<MoveToPoint>(q.front()).target_mkm;
    } else {
      const Id body_id = std::get<MoveToBody>(q.front()).body_id;
      const auto* body = find_ptr(state_.bodies, body_id);
      if (!body) {
        q.erase(q.begin());
        continue;
      }
      target = body->position_mkm;
    }

    const Vec2 delta = target - ship.position_mkm;
    const double dist = delta.length();
    if (dist < 1e-6) {
      q.erase(q.begin());
      continue;
    }

    const double max_step = mkm_per_day_from_speed(ship.speed_km_s, cfg_.seconds_per_day);
    if (max_step <= 0.0) continue;
    if (dist <= max_step) {
      ship.position_mkm = target;
      q.erase(q.begin());
      continue;
    }

    const Vec2 dir = delta.normalized();
    ship.position_mkm += dir * max_step;
  }
}

} // namespace nebula4x
