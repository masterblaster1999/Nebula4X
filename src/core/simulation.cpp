#include "nebula4x/core/simulation.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <unordered_map>

#include "nebula4x/core/scenario.h"
#include "nebula4x/util/log.h"

namespace nebula4x {
namespace {

constexpr double kTwoPi = 6.283185307179586476925286766559;

double mkm_per_day_from_speed(double speed_km_s, double seconds_per_day) {
  const double km_per_day = speed_km_s * seconds_per_day;
  return km_per_day / 1.0e6; // million km
}

template <typename T>
void push_unique(std::vector<T>& v, const T& x) {
  if (std::find(v.begin(), v.end(), x) == v.end()) v.push_back(x);
}

bool vec_contains(const std::vector<std::string>& v, const std::string& x) {
  return std::find(v.begin(), v.end(), x) != v.end();
}

bool faction_has_tech(const Faction& f, const std::string& tech_id) {
  return std::find(f.known_techs.begin(), f.known_techs.end(), tech_id) != f.known_techs.end();
}

} // namespace

Simulation::Simulation(ContentDB content, SimConfig cfg) : content_(std::move(content)), cfg_(cfg) {
  new_game();
}

const ShipDesign* Simulation::find_design(const std::string& design_id) const {
  if (auto it = state_.custom_designs.find(design_id); it != state_.custom_designs.end()) return &it->second;
  if (auto it = content_.designs.find(design_id); it != content_.designs.end()) return &it->second;
  return nullptr;
}

bool Simulation::is_design_buildable_for_faction(Id faction_id, const std::string& design_id) const {
  const auto* d = find_design(design_id);
  if (!d) return false;

  const auto* fac = find_ptr(state_.factions, faction_id);
  if (!fac) return true; // Debug-friendly fallback.

  for (const auto& cid : d->components) {
    if (!vec_contains(fac->unlocked_components, cid)) return false;
  }
  return true;
}

bool Simulation::is_installation_buildable_for_faction(Id faction_id, const std::string& installation_id) const {
  if (content_.installations.find(installation_id) == content_.installations.end()) return false;

  const auto* fac = find_ptr(state_.factions, faction_id);
  if (!fac) return true; // Debug-friendly fallback.

  return vec_contains(fac->unlocked_installations, installation_id);
}

double Simulation::construction_points_per_day(const Colony& colony) const {
  // Baseline: population drives a tiny amount of "free" construction.
  // This keeps the early prototype playable even before dedicated industry is modeled.
  double total = std::max(0.0, colony.population_millions * 0.01);

  for (const auto& [inst_id, count] : colony.installations) {
    if (count <= 0) continue;
    const auto it = content_.installations.find(inst_id);
    if (it == content_.installations.end()) continue;
    const double per_day = it->second.construction_points_per_day;
    if (per_day <= 0.0) continue;
    total += per_day * static_cast<double>(count);
  }

  return total;
}

void Simulation::apply_design_stats_to_ship(Ship& ship) {
  const ShipDesign* d = find_design(ship.design_id);
  if (!d) {
    ship.speed_km_s = 0.0;
    if (ship.hp <= 0.0) ship.hp = 1.0;
    return;
  }

  ship.speed_km_s = d->speed_km_s;
  if (ship.hp <= 0.0) ship.hp = d->max_hp;
  // Clamp just in case content changed between versions.
  ship.hp = std::clamp(ship.hp, 0.0, d->max_hp);
}

bool Simulation::upsert_custom_design(ShipDesign design, std::string* error) {
  auto fail = [&](const std::string& msg) {
    if (error) *error = msg;
    return false;
  };

  if (design.id.empty()) return fail("Design id is empty");
  if (content_.designs.find(design.id) != content_.designs.end()) {
    return fail("Design id conflicts with built-in design: " + design.id);
  }
  if (design.name.empty()) design.name = design.id;

  // Validate components and (re)derive stats.
  double mass = 0.0;
  double speed = 0.0;
  double cargo = 0.0;
  double sensor = 0.0;
  double weapon_damage = 0.0;
  double weapon_range = 0.0;
  double hp_bonus = 0.0;

  for (const auto& cid : design.components) {
    auto it = content_.components.find(cid);
    if (it == content_.components.end()) return fail("Unknown component id: " + cid);
    const auto& c = it->second;

    mass += c.mass_tons;
    speed = std::max(speed, c.speed_km_s);
    cargo += c.cargo_tons;
    sensor = std::max(sensor, c.sensor_range_mkm);

    if (c.type == ComponentType::Weapon) {
      weapon_damage += c.weapon_damage;
      weapon_range = std::max(weapon_range, c.weapon_range_mkm);
    }

    hp_bonus += c.hp_bonus;
  }

  design.mass_tons = mass;
  design.speed_km_s = speed;
  design.cargo_tons = cargo;
  design.sensor_range_mkm = sensor;
  design.weapon_damage = weapon_damage;
  design.weapon_range_mkm = weapon_range;
  design.max_hp = std::max(1.0, mass * 2.0 + hp_bonus);

  state_.custom_designs[design.id] = std::move(design);
  return true;
}

void Simulation::initialize_unlocks_for_faction(Faction& f) {
  // Installations present on colonies belonging to this faction.
  for (const auto& [_, col] : state_.colonies) {
    if (col.faction_id != f.id) continue;
    for (const auto& [inst_id, _count] : col.installations) push_unique(f.unlocked_installations, inst_id);
  }

  // Components present on existing ships belonging to this faction.
  for (const auto& [_, ship] : state_.ships) {
    if (ship.faction_id != f.id) continue;
    if (const auto* d = find_design(ship.design_id)) {
      for (const auto& cid : d->components) push_unique(f.unlocked_components, cid);
    }
  }

  // Effects of already-known tech.
  for (const auto& tech_id : f.known_techs) {
    auto tit = content_.techs.find(tech_id);
    if (tit == content_.techs.end()) continue;
    for (const auto& eff : tit->second.effects) {
      if (eff.type == "unlock_component") push_unique(f.unlocked_components, eff.value);
      if (eff.type == "unlock_installation") push_unique(f.unlocked_installations, eff.value);
    }
  }
}

void Simulation::new_game() {
  state_ = make_sol_scenario();

  // Apply design stats to ships and seed unlock lists.
  for (auto& [_, ship] : state_.ships) {
    apply_design_stats_to_ship(ship);
  }

  // Seed unlock lists based on starting assets + known tech effects.
  for (auto& [_, f] : state_.factions) initialize_unlocks_for_faction(f);

  recompute_body_positions();
}

void Simulation::load_game(GameState loaded) {
  state_ = std::move(loaded);

  // Re-derive custom design stats in case the save came from an older version
  // (or content packs changed). We keep any invalid designs but warn.
  if (!state_.custom_designs.empty()) {
    std::vector<ShipDesign> designs;
    designs.reserve(state_.custom_designs.size());
    for (const auto& [_, d] : state_.custom_designs) designs.push_back(d);
    state_.custom_designs.clear();
    for (auto& d : designs) {
      std::string err;
      if (!upsert_custom_design(d, &err)) {
        nebula4x::log::warn(std::string("Custom design '") + d.id + "' could not be re-derived: " + err);
        state_.custom_designs[d.id] = d; // keep as-is
      }
    }
  }

  // Re-derive ship stats in case content changed.
  for (auto& [_, ship] : state_.ships) {
    apply_design_stats_to_ship(ship);
  }

  // Ensure unlock lists include all current ships/colonies + known-tech effects.
  for (auto& [_, f] : state_.factions) {
    initialize_unlocks_for_faction(f);
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

bool Simulation::issue_travel_via_jump(Id ship_id, Id jump_point_id) {
  auto* ship = find_ptr(state_.ships, ship_id);
  if (!ship) return false;
  if (!find_ptr(state_.jump_points, jump_point_id)) return false;
  auto& orders = state_.ship_orders[ship_id];
  orders.queue.push_back(TravelViaJump{jump_point_id});
  return true;
}

bool Simulation::issue_attack_ship(Id attacker_ship_id, Id target_ship_id) {
  if (attacker_ship_id == target_ship_id) return false;
  auto* attacker = find_ptr(state_.ships, attacker_ship_id);
  if (!attacker) return false;
  if (!find_ptr(state_.ships, target_ship_id)) return false;
  auto& orders = state_.ship_orders[attacker_ship_id];
  orders.queue.push_back(AttackShip{target_ship_id});
  return true;
}

bool Simulation::enqueue_build(Id colony_id, const std::string& design_id) {
  auto* colony = find_ptr(state_.colonies, colony_id);
  if (!colony) return false;
  const auto* d = find_design(design_id);
  if (!d) return false;
  if (!is_design_buildable_for_faction(colony->faction_id, design_id)) return false;
  BuildOrder bo;
  bo.design_id = design_id;
  bo.tons_remaining = std::max(1.0, d->mass_tons);
  colony->shipyard_queue.push_back(bo);
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
  tick_research();
  tick_shipyards();
  tick_construction();
  tick_ships();
  tick_combat();
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

void Simulation::tick_research() {
  // Generate RP from colonies.
  for (const auto& [_, colony] : state_.colonies) {
    auto* fac = find_ptr(state_.factions, colony.faction_id);
    if (!fac) continue;

    for (const auto& [inst_id, count] : colony.installations) {
      auto dit = content_.installations.find(inst_id);
      if (dit == content_.installations.end()) continue;
      const double rp = dit->second.research_points_per_day * static_cast<double>(count);
      if (rp > 0.0) fac->research_points += rp;
    }
  }

  // Spend RP into active research projects.
  for (auto& [_, fac] : state_.factions) {
    // Ensure active project if we have a queue.
    auto select_next = [&]() {
      while (!fac.research_queue.empty()) {
        fac.active_research_id = fac.research_queue.front();
        fac.research_queue.erase(fac.research_queue.begin());
        fac.active_research_progress = 0.0;
        // Skip if already known.
        if (!faction_has_tech(fac, fac.active_research_id)) break;
        fac.active_research_id.clear();
      }
    };

    if (fac.active_research_id.empty()) select_next();

    while (fac.research_points > 0.0) {
      if (fac.active_research_id.empty()) break;
      if (faction_has_tech(fac, fac.active_research_id)) {
        fac.active_research_id.clear();
        fac.active_research_progress = 0.0;
        select_next();
        continue;
      }

      auto tit = content_.techs.find(fac.active_research_id);
      if (tit == content_.techs.end()) {
        // Unknown tech id in queue.
        fac.active_research_id.clear();
        fac.active_research_progress = 0.0;
        select_next();
        continue;
      }

      const auto& tech = tit->second;
      const double remaining = std::max(0.0, tech.cost - fac.active_research_progress);
      if (remaining <= 0.0) {
        // Complete.
        fac.known_techs.push_back(tech.id);
        for (const auto& eff : tech.effects) {
          if (eff.type == "unlock_component") push_unique(fac.unlocked_components, eff.value);
          if (eff.type == "unlock_installation") push_unique(fac.unlocked_installations, eff.value);
        }
        nebula4x::log::info("Research complete: " + tech.name + " (" + tech.id + ")");
        fac.active_research_id.clear();
        fac.active_research_progress = 0.0;
        select_next();
        continue;
      }

      const double spend = std::min(fac.research_points, remaining);
      fac.research_points -= spend;
      fac.active_research_progress += spend;

      // Check completion.
      if (fac.active_research_progress + 1e-9 >= tech.cost && tech.cost > 0.0) {
        // Loop will complete next iteration.
        continue;
      }

      continue; // keep spending until bank is empty (same-day research batching).
    }
  }
}

void Simulation::tick_shipyards() {
  const auto it_def = content_.installations.find("shipyard");
  if (it_def == content_.installations.end()) return;

  const InstallationDef& shipyard_def = it_def->second;
  const double base_rate = shipyard_def.build_rate_tons_per_day;
  if (base_rate <= 0.0) return;

  const auto& costs_per_ton = shipyard_def.build_costs_per_ton;

  auto max_build_by_minerals = [&](const Colony& colony, double desired_tons) {
    double max_tons = desired_tons;
    for (const auto& [mineral, cost_per_ton] : costs_per_ton) {
      if (cost_per_ton <= 0.0) continue;
      const auto it = colony.minerals.find(mineral);
      const double available = (it == colony.minerals.end()) ? 0.0 : it->second;
      max_tons = std::min(max_tons, available / cost_per_ton);
    }
    return max_tons;
  };

  auto consume_minerals = [&](Colony& colony, double built_tons) {
    for (const auto& [mineral, cost_per_ton] : costs_per_ton) {
      if (cost_per_ton <= 0.0) continue;
      const double cost = built_tons * cost_per_ton;
      colony.minerals[mineral] = std::max(0.0, colony.minerals[mineral] - cost);
    }
  };

  for (auto& [_, colony] : state_.colonies) {
    const int yards = colony.installations["shipyard"];
    if (yards <= 0) continue;

    double capacity_tons = base_rate * static_cast<double>(yards);

    while (capacity_tons > 1e-9 && !colony.shipyard_queue.empty()) {
      auto& bo = colony.shipyard_queue.front();

      double build_tons = std::min(capacity_tons, bo.tons_remaining);

      // Apply mineral constraints (if costs are configured).
      if (!costs_per_ton.empty()) {
        build_tons = max_build_by_minerals(colony, build_tons);
      }

      if (build_tons <= 1e-9) {
        // Stalled due to lack of minerals (or zero capacity).
        break;
      }

      // Spend minerals and progress the build.
      if (!costs_per_ton.empty()) consume_minerals(colony, build_tons);
      bo.tons_remaining -= build_tons;
      capacity_tons -= build_tons;

      if (bo.tons_remaining > 1e-9) {
        // Not finished; all remaining capacity (if any) will be unused this day.
        break;
      }

      // Build complete: spawn ship at colony body position.
      const auto* design = find_design(bo.design_id);
      if (!design) {
        nebula4x::log::warn(std::string("Unknown design in build queue: ") + bo.design_id);
      } else {
        const auto* body = find_ptr(state_.bodies, colony.body_id);
        if (!body) {
          nebula4x::log::warn("Colony " + std::to_string(colony.id) + " has missing body " +
                             std::to_string(colony.body_id));
        } else {
          const auto* sys = find_ptr(state_.systems, body->system_id);
          if (!sys) {
            nebula4x::log::warn("Body " + std::to_string(body->id) + " has missing system " +
                               std::to_string(body->system_id));
          } else {
            Ship sh;
            sh.id = allocate_id(state_);
            sh.faction_id = colony.faction_id;
            sh.system_id = body->system_id;
            sh.design_id = bo.design_id;
            sh.position_mkm = body->position_mkm;

            apply_design_stats_to_ship(sh);

            // Simple name numbering.
            sh.name = design->name + " #" + std::to_string(sh.id);

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

void Simulation::tick_construction() {
  for (auto& [_, colony] : state_.colonies) {
    // Cache CP/day at the start of the tick so newly completed factories don't
    // immediately grant extra CP within the same day.
    double cp_available = construction_points_per_day(colony);
    if (cp_available <= 1e-9) continue;

    auto can_pay_minerals = [&](const InstallationDef& def) {
      for (const auto& [mineral, cost] : def.build_costs) {
        if (cost <= 0.0) continue;
        const auto it = colony.minerals.find(mineral);
        const double have = (it == colony.minerals.end()) ? 0.0 : it->second;
        if (have + 1e-9 < cost) return false;
      }
      return true;
    };

    auto pay_minerals = [&](const InstallationDef& def) {
      for (const auto& [mineral, cost] : def.build_costs) {
        if (cost <= 0.0) continue;
        colony.minerals[mineral] = std::max(0.0, colony.minerals[mineral] - cost);
      }
    };

    while (cp_available > 1e-9 && !colony.construction_queue.empty()) {
      auto& ord = colony.construction_queue.front();

      if (ord.quantity_remaining <= 0) {
        colony.construction_queue.erase(colony.construction_queue.begin());
        continue;
      }

      auto it_def = content_.installations.find(ord.installation_id);
      if (it_def == content_.installations.end()) {
        nebula4x::log::warn("Unknown installation in construction queue: " + ord.installation_id);
        colony.construction_queue.erase(colony.construction_queue.begin());
        continue;
      }
      const InstallationDef& def = it_def->second;

      // If we haven't started the current unit yet, attempt to pay minerals.
      if (!ord.minerals_paid) {
        if (!can_pay_minerals(def)) {
          // Stalled due to missing minerals.
          break;
        }
        pay_minerals(def);
        ord.minerals_paid = true;
        ord.cp_remaining = std::max(0.0, def.construction_cost);

        // Instant-build installations (cost 0 CP) complete immediately.
        if (ord.cp_remaining <= 1e-9) {
          colony.installations[def.id] += 1;
          ord.quantity_remaining -= 1;
          ord.minerals_paid = false;
          ord.cp_remaining = 0.0;

          nebula4x::log::info("Constructed " + def.name + " at " + colony.name);

          if (ord.quantity_remaining <= 0) {
            colony.construction_queue.erase(colony.construction_queue.begin());
          }
          continue;
        }
      }

      // Spend construction points.
      const double spend = std::min(cp_available, ord.cp_remaining);
      ord.cp_remaining -= spend;
      cp_available -= spend;

      if (ord.cp_remaining <= 1e-9) {
        colony.installations[def.id] += 1;
        ord.quantity_remaining -= 1;
        ord.minerals_paid = false;
        ord.cp_remaining = 0.0;

        nebula4x::log::info("Constructed " + def.name + " at " + colony.name);

        if (ord.quantity_remaining <= 0) {
          colony.construction_queue.erase(colony.construction_queue.begin());
        }
        continue;
      }

      // Not complete; no more CP remains (spend used the remaining CP).
      break;
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
    double desired_range = 0.0; // used for attack orders

    if (std::holds_alternative<MoveToPoint>(q.front())) {
      target = std::get<MoveToPoint>(q.front()).target_mkm;
    } else if (std::holds_alternative<MoveToBody>(q.front())) {
      const Id body_id = std::get<MoveToBody>(q.front()).body_id;
      const auto* body = find_ptr(state_.bodies, body_id);
      if (!body) {
        q.erase(q.begin());
        continue;
      }
      if (body->system_id != ship.system_id) {
        // Can't path across systems yet; drop order.
        q.erase(q.begin());
        continue;
      }
      target = body->position_mkm;
    } else if (std::holds_alternative<TravelViaJump>(q.front())) {
      const Id jump_id = std::get<TravelViaJump>(q.front()).jump_point_id;
      const auto* jp = find_ptr(state_.jump_points, jump_id);
      if (!jp || jp->system_id != ship.system_id) {
        q.erase(q.begin());
        continue;
      }
      target = jp->position_mkm;
    } else if (std::holds_alternative<AttackShip>(q.front())) {
      const Id target_id = std::get<AttackShip>(q.front()).target_ship_id;
      const auto* tgt = find_ptr(state_.ships, target_id);
      if (!tgt || tgt->system_id != ship.system_id) {
        q.erase(q.begin());
        continue;
      }
      target = tgt->position_mkm;
      const auto* d = find_design(ship.design_id);
      const double w_range = d ? d->weapon_range_mkm : 0.0;
      desired_range = (w_range > 0.0) ? (w_range * 0.9) : 0.1;
    }

    const Vec2 delta = target - ship.position_mkm;
    const double dist = delta.length();

    // If this is a normal move order, reaching target completes it.
    const bool is_attack = std::holds_alternative<AttackShip>(q.front());
    const bool is_jump = std::holds_alternative<TravelViaJump>(q.front());

    if (!is_attack && dist < 1e-6) {
      // done
      q.erase(q.begin());
      continue;
    }

    // For attack orders: stop when within desired range.
    if (is_attack && dist <= desired_range) {
      continue;
    }

    const double max_step = mkm_per_day_from_speed(ship.speed_km_s, cfg_.seconds_per_day);
    if (max_step <= 0.0) continue;

    double step = max_step;
    if (is_attack) {
      step = std::min(step, std::max(0.0, dist - desired_range));
      if (step <= 0.0) continue;
    }

    if (dist <= step) {
      ship.position_mkm = target;

      if (is_jump) {
        // Transit the jump point.
        const Id jump_id = std::get<TravelViaJump>(q.front()).jump_point_id;
        const auto* jp = find_ptr(state_.jump_points, jump_id);
        if (jp && jp->system_id == ship.system_id && jp->linked_jump_id != kInvalidId) {
          const auto* dest = find_ptr(state_.jump_points, jp->linked_jump_id);
          if (dest) {
            const Id old_sys = ship.system_id;
            const Id new_sys = dest->system_id;

            // Remove ship from old system list.
            if (auto* sys_old = find_ptr(state_.systems, old_sys)) {
              sys_old->ships.erase(std::remove(sys_old->ships.begin(), sys_old->ships.end(), ship_id), sys_old->ships.end());
            }

            ship.system_id = new_sys;
            ship.position_mkm = dest->position_mkm;

            if (auto* sys_new = find_ptr(state_.systems, new_sys)) {
              sys_new->ships.push_back(ship_id);
            }

            nebula4x::log::info("Ship " + ship.name + " transited jump point " + jp->name + " -> " +
                               (find_ptr(state_.systems, new_sys) ? find_ptr(state_.systems, new_sys)->name : std::string("(unknown)")));
          }
        }

        // Jump order completes.
        q.erase(q.begin());
      } else if (!is_attack) {
        q.erase(q.begin());
      }

      continue;
    }

    const Vec2 dir = delta.normalized();
    ship.position_mkm += dir * step;
  }
}

void Simulation::tick_combat() {
  // Simple day-based combat:
  // - Each armed ship fires once per day at either its explicit target (AttackShip order)
  //   or the closest hostile ship within range.
  // - Damage is applied simultaneously.

  std::unordered_map<Id, double> incoming_damage;

  auto is_hostile = [&](const Ship& a, const Ship& b) { return a.faction_id != b.faction_id; };

  for (const auto& [aid, attacker] : state_.ships) {
    const auto* ad = find_design(attacker.design_id);
    if (!ad) continue;
    if (ad->weapon_damage <= 0.0 || ad->weapon_range_mkm <= 0.0) continue;

    // Candidate targets: ships in same system and hostile.
    Id chosen = kInvalidId;
    double chosen_dist = 1e300;

    // If explicit target exists (front order), prefer it.
    auto oit = state_.ship_orders.find(aid);
    if (oit != state_.ship_orders.end() && !oit->second.queue.empty() &&
        std::holds_alternative<AttackShip>(oit->second.queue.front())) {
      const Id tid = std::get<AttackShip>(oit->second.queue.front()).target_ship_id;
      const auto* tgt = find_ptr(state_.ships, tid);
      if (tgt && tgt->system_id == attacker.system_id && is_hostile(attacker, *tgt)) {
        const double dist = (tgt->position_mkm - attacker.position_mkm).length();
        if (dist <= ad->weapon_range_mkm) {
          chosen = tid;
          chosen_dist = dist;
        }
      }
    }

    // Otherwise, pick closest hostile within range.
    if (chosen == kInvalidId) {
      for (const auto& [bid, target] : state_.ships) {
        if (bid == aid) continue;
        if (target.system_id != attacker.system_id) continue;
        if (!is_hostile(attacker, target)) continue;

        const double dist = (target.position_mkm - attacker.position_mkm).length();
        if (dist > ad->weapon_range_mkm) continue;
        if (dist < chosen_dist) {
          chosen = bid;
          chosen_dist = dist;
        }
      }
    }

    if (chosen != kInvalidId) {
      incoming_damage[chosen] += ad->weapon_damage;
    }
  }

  if (incoming_damage.empty()) return;

  std::vector<Id> destroyed;
  destroyed.reserve(incoming_damage.size());

  for (const auto& [tid, dmg] : incoming_damage) {
    auto* tgt = find_ptr(state_.ships, tid);
    if (!tgt) continue;
    tgt->hp -= dmg;
    if (tgt->hp <= 0.0) destroyed.push_back(tid);
  }

  for (Id dead_id : destroyed) {
    auto it = state_.ships.find(dead_id);
    if (it == state_.ships.end()) continue;

    const Id sys_id = it->second.system_id;
    const std::string name = it->second.name;

    // Remove from system.
    if (auto* sys = find_ptr(state_.systems, sys_id)) {
      sys->ships.erase(std::remove(sys->ships.begin(), sys->ships.end(), dead_id), sys->ships.end());
    }

    state_.ship_orders.erase(dead_id);
    state_.ships.erase(dead_id);

    nebula4x::log::warn("Ship destroyed: " + name);
  }
}

} // namespace nebula4x
