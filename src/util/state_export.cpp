#include "nebula4x/util/state_export.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

#include "nebula4x/core/game_state.h"
#include "nebula4x/core/orders.h"
#include "nebula4x/util/json.h"

namespace nebula4x {
namespace {

template <typename Map>
std::vector<typename Map::key_type> sorted_keys(const Map& m) {
  std::vector<typename Map::key_type> keys;
  keys.reserve(m.size());
  for (const auto& [k, _] : m) keys.push_back(k);
  std::sort(keys.begin(), keys.end());
  return keys;
}

std::string faction_name(const GameState& s, Id id) {
  if (id == kInvalidId) return {};
  const auto it = s.factions.find(id);
  return (it != s.factions.end()) ? it->second.name : std::string{};
}

std::string system_name(const GameState& s, Id id) {
  if (id == kInvalidId) return {};
  const auto it = s.systems.find(id);
  return (it != s.systems.end()) ? it->second.name : std::string{};
}

std::string body_name(const GameState& s, Id id) {
  if (id == kInvalidId) return {};
  const auto it = s.bodies.find(id);
  return (it != s.bodies.end()) ? it->second.name : std::string{};
}

const char* body_type_label(BodyType t) {
  switch (t) {
    case BodyType::Star:
      return "star";
    case BodyType::Planet:
      return "planet";
    case BodyType::Moon:
      return "moon";
    case BodyType::Asteroid:
      return "asteroid";
    case BodyType::Comet:
      return "comet";
    case BodyType::GasGiant:
      return "gas_giant";
  }
  return "unknown";
}

const ShipDesign* find_design(const GameState& s, const ContentDB* content, const std::string& id) {
  if (id.empty()) return nullptr;

  const auto it_custom = s.custom_designs.find(id);
  if (it_custom != s.custom_designs.end()) return &it_custom->second;

  if (!content) return nullptr;
  const auto it = content->designs.find(id);
  if (it == content->designs.end()) return nullptr;
  return &it->second;
}

std::string design_name(const GameState& s, const ContentDB* content, const std::string& id) {
  const auto* d = find_design(s, content, id);
  return d ? d->name : std::string{};
}

const InstallationDef* find_installation(const ContentDB* content, const std::string& id) {
  if (!content || id.empty()) return nullptr;
  const auto it = content->installations.find(id);
  if (it == content->installations.end()) return nullptr;
  return &it->second;
}

std::string installation_name(const ContentDB* content, const std::string& id) {
  const auto* inst = find_installation(content, id);
  return inst ? inst->name : std::string{};
}

double sum_positive(const std::unordered_map<std::string, double>& m) {
  double total = 0.0;
  for (const auto& [_, v] : m) {
    if (v > 0.0) total += v;
  }
  return total;
}

json::Object vec2_to_object(const Vec2& v) {
  json::Object o;
  o["x"] = v.x;
  o["y"] = v.y;
  return o;
}

json::Array power_priority_to_json(const ShipPowerPolicy& policy) {
  json::Array pr;
  pr.reserve(policy.priority.size());
  for (PowerSubsystem s : policy.priority) {
    pr.push_back(power_subsystem_to_string(s));
  }
  return pr;
}

} // namespace

std::string ships_to_json(const GameState& state, const ContentDB* content) {
  json::Array out;
  out.reserve(state.ships.size());

  for (Id sid : sorted_keys(state.ships)) {
    const auto it = state.ships.find(sid);
    if (it == state.ships.end()) continue;
    const Ship& sh = it->second;

    const ShipDesign* d = find_design(state, content, sh.design_id);

    json::Object obj;
    obj["id"] = static_cast<double>(sid);
    obj["name"] = sh.name;
    obj["faction_id"] = static_cast<double>(sh.faction_id);
    obj["faction"] = faction_name(state, sh.faction_id);
    obj["system_id"] = static_cast<double>(sh.system_id);
    obj["system"] = system_name(state, sh.system_id);

    obj["design_id"] = sh.design_id;
    obj["design"] = design_name(state, content, sh.design_id);

    obj["position_mkm"] = vec2_to_object(sh.position_mkm);

    obj["speed_km_s"] = sh.speed_km_s;
    obj["hp"] = sh.hp;
    obj["shields"] = std::max(0.0, sh.shields);
    obj["fuel_tons"] = std::max(0.0, sh.fuel_tons);
    obj["troops"] = std::max(0.0, sh.troops);

    obj["design_mass_tons"] = d ? d->mass_tons : 0.0;
    obj["design_fuel_capacity_tons"] = d ? d->fuel_capacity_tons : 0.0;
    obj["design_fuel_use_per_mkm"] = d ? d->fuel_use_per_mkm : 0.0;
    obj["design_cargo_tons"] = d ? d->cargo_tons : 0.0;
    obj["design_sensor_range_mkm"] = d ? d->sensor_range_mkm : 0.0;
    obj["design_colony_capacity_millions"] = d ? d->colony_capacity_millions : 0.0;
    obj["design_troop_capacity"] = d ? d->troop_capacity : 0.0;
    obj["design_power_generation"] = d ? d->power_generation : 0.0;
    obj["design_power_use_total"] = d ? d->power_use_total : 0.0;
    obj["design_power_use_engines"] = d ? d->power_use_engines : 0.0;
    obj["design_power_use_sensors"] = d ? d->power_use_sensors : 0.0;
    obj["design_power_use_weapons"] = d ? d->power_use_weapons : 0.0;
    obj["design_power_use_shields"] = d ? d->power_use_shields : 0.0;
    obj["design_max_hp"] = d ? d->max_hp : 0.0;
    obj["design_max_shields"] = d ? d->max_shields : 0.0;
    obj["design_shield_regen_per_day"] = d ? d->shield_regen_per_day : 0.0;
    obj["design_weapon_damage"] = d ? d->weapon_damage : 0.0;
    obj["design_weapon_range_mkm"] = d ? d->weapon_range_mkm : 0.0;

    // Runtime power settings
    obj["power_policy_engines_enabled"] = sh.power_policy.engines_enabled;
    obj["power_policy_shields_enabled"] = sh.power_policy.shields_enabled;
    obj["power_policy_weapons_enabled"] = sh.power_policy.weapons_enabled;
    obj["power_policy_sensors_enabled"] = sh.power_policy.sensors_enabled;
    obj["power_policy_priority"] = power_priority_to_json(sh.power_policy);

    if (d) {
      const auto p = compute_power_allocation(d->power_generation, d->power_use_engines, d->power_use_shields,
                                             d->power_use_weapons, d->power_use_sensors, sh.power_policy);
      obj["power_available"] = p.available;
      obj["power_engines_online"] = p.engines_online;
      obj["power_shields_online"] = p.shields_online;
      obj["power_weapons_online"] = p.weapons_online;
      obj["power_sensors_online"] = p.sensors_online;
    } else {
      obj["power_available"] = 0.0;
      obj["power_engines_online"] = false;
      obj["power_shields_online"] = false;
      obj["power_weapons_online"] = false;
      obj["power_sensors_online"] = false;
    }

    json::Object cargo;
    for (const auto& [k, v] : sh.cargo) cargo[k] = v;
    obj["cargo"] = std::move(cargo);
    obj["cargo_used_tons"] = sum_positive(sh.cargo);
    obj["cargo_capacity_tons"] = d ? d->cargo_tons : 0.0;

    // Orders (human-readable strings; still machine-parseable as a list).
    const auto it_orders = state.ship_orders.find(sid);
    if (it_orders != state.ship_orders.end()) {
      const ShipOrders& so = it_orders->second;
      obj["orders_repeat"] = so.repeat;
      obj["orders_repeat_count_remaining"] = static_cast<double>(so.repeat_count_remaining);

      json::Array q;
      q.reserve(so.queue.size());
      for (const auto& ord : so.queue) q.emplace_back(order_to_string(ord));
      obj["order_queue"] = std::move(q);

      json::Array rt;
      rt.reserve(so.repeat_template.size());
      for (const auto& ord : so.repeat_template) rt.emplace_back(order_to_string(ord));
      obj["repeat_template"] = std::move(rt);
    } else {
      obj["orders_repeat"] = false;
      obj["orders_repeat_count_remaining"] = 0.0;
      obj["order_queue"] = json::Array{};
      obj["repeat_template"] = json::Array{};
    }

    out.emplace_back(std::move(obj));
  }

  std::string json_text = json::stringify(json::array(std::move(out)), 2);
  json_text.push_back('\n');
  return json_text;
}

std::string colonies_to_json(const GameState& state, const ContentDB* content) {
  json::Array out;
  out.reserve(state.colonies.size());

  for (Id cid : sorted_keys(state.colonies)) {
    const auto it = state.colonies.find(cid);
    if (it == state.colonies.end()) continue;
    const Colony& c = it->second;

    const Body* body = find_ptr(state.bodies, c.body_id);
    const Id sys_id = body ? body->system_id : kInvalidId;

    // Compute aggregate per-day values from installations.
    std::unordered_map<std::string, double> prod;
    std::unordered_map<std::string, double> cons;
    std::unordered_map<std::string, double> mining_prod;
    double construction_points_per_day = 0.0;
    double shipyard_capacity_tons_per_day = 0.0;
    double terraforming_points_per_day = 0.0;
    double troop_training_points_per_day = 0.0;
    double fortification_points = 0.0;

    if (content) {
      for (const auto& [inst_id, count] : c.installations) {
        if (count <= 0) continue;
        const InstallationDef* def = find_installation(content, inst_id);
        if (!def) continue;

        const bool mining = def->mining;
        for (const auto& [mineral, per_day] : def->produces_per_day) {
          const double amt = per_day * static_cast<double>(count);
          prod[mineral] += amt;
          if (mining) mining_prod[mineral] += amt;
        }
        for (const auto& [mineral, per_day] : def->consumes_per_day) {
          const double amt = per_day * static_cast<double>(count);
          cons[mineral] += amt;
        }


        construction_points_per_day += def->construction_points_per_day * static_cast<double>(count);
        shipyard_capacity_tons_per_day += def->build_rate_tons_per_day * static_cast<double>(count);
        terraforming_points_per_day += def->terraforming_points_per_day * static_cast<double>(count);
        troop_training_points_per_day += def->troop_training_points_per_day * static_cast<double>(count);
        fortification_points += def->fortification_points * static_cast<double>(count);
      }
    }

    json::Object obj;
    obj["id"] = static_cast<double>(cid);
    obj["name"] = c.name;
    obj["faction_id"] = static_cast<double>(c.faction_id);
    obj["faction"] = faction_name(state, c.faction_id);

    obj["system_id"] = static_cast<double>(sys_id);
    obj["system"] = system_name(state, sys_id);

    obj["body_id"] = static_cast<double>(c.body_id);
    obj["body"] = body_name(state, c.body_id);

    obj["population_millions"] = c.population_millions;
    obj["ground_forces"] = std::max(0.0, c.ground_forces);
    obj["troop_training_queue"] = std::max(0.0, c.troop_training_queue);

    json::Object minerals;
    for (const auto& [k, v] : c.minerals) minerals[k] = v;
    obj["minerals"] = std::move(minerals);

    if (!c.mineral_reserves.empty()) {
      json::Object reserves;
      for (const auto& k : sorted_keys(c.mineral_reserves)) {
        reserves[k] = c.mineral_reserves.at(k);
      }
      obj["mineral_reserves"] = std::move(reserves);
    }

    // Installations as an array (id/name/count) for ease of consumption.
    json::Array inst_arr;
    inst_arr.reserve(c.installations.size());
    {
      std::vector<std::string> inst_ids;
      inst_ids.reserve(c.installations.size());
      for (const auto& [id, _] : c.installations) inst_ids.push_back(id);
      std::sort(inst_ids.begin(), inst_ids.end());
      for (const auto& inst_id : inst_ids) {
        const int count = c.installations.at(inst_id);
        json::Object inst_obj;
        inst_obj["id"] = inst_id;
        inst_obj["name"] = installation_name(content, inst_id);
        inst_obj["count"] = static_cast<double>(count);
        inst_arr.emplace_back(std::move(inst_obj));
      }
    }
    obj["installations"] = std::move(inst_arr);

    json::Object prod_obj;
    for (const auto& [k, v] : prod) prod_obj[k] = v;
    obj["mineral_production_per_day"] = std::move(prod_obj);

    json::Object cons_obj;
    for (const auto& [k, v] : cons) cons_obj[k] = v;
    obj["mineral_consumption_per_day"] = std::move(cons_obj);

    // Finite mineral deposits (if present on the underlying body).
    if (body && !body->mineral_deposits.empty()) {
      json::Object dep_obj;
      for (const auto& mineral : sorted_keys(body->mineral_deposits)) {
        dep_obj[mineral] = body->mineral_deposits.at(mineral);
      }
      obj["body_mineral_deposits"] = std::move(dep_obj);

      // Per-colony mining potential + depletion ETA (days) based on current mining installations.
      if (!mining_prod.empty()) {
        json::Object mining_obj;
        for (const auto& mineral : sorted_keys(mining_prod)) {
          mining_obj[mineral] = mining_prod.at(mineral);
        }
        obj["mineral_mining_potential_per_day"] = std::move(mining_obj);

        json::Object eta_obj;
        for (const auto& mineral : sorted_keys(body->mineral_deposits)) {
          const double dep = body->mineral_deposits.at(mineral);
          const auto it_rate = mining_prod.find(mineral);
          const double rate = (it_rate != mining_prod.end()) ? it_rate->second : 0.0;
          if (rate > 1e-9) {
            eta_obj[mineral] = dep / rate;
          } else {
            eta_obj[mineral] = nullptr;
          }
        }
        obj["mineral_depletion_eta_days"] = std::move(eta_obj);
      }
    }

    obj["construction_points_per_day"] = construction_points_per_day;
    obj["shipyard_capacity_tons_per_day"] = shipyard_capacity_tons_per_day;
    obj["terraforming_points_per_day"] = terraforming_points_per_day;
    obj["troop_training_points_per_day"] = troop_training_points_per_day;
    obj["fortification_points"] = fortification_points;

    // Shipyard queue
    json::Array shipyard_q;
    shipyard_q.reserve(c.shipyard_queue.size());
    for (const auto& bo : c.shipyard_queue) {
      json::Object bo_obj;
      bo_obj["design_id"] = bo.design_id;
      bo_obj["design"] = design_name(state, content, bo.design_id);
      bo_obj["tons_remaining"] = bo.tons_remaining;
      shipyard_q.emplace_back(std::move(bo_obj));
    }
    obj["shipyard_queue"] = std::move(shipyard_q);

    // Construction queue
    json::Array constr_q;
    constr_q.reserve(c.construction_queue.size());
    for (const auto& io : c.construction_queue) {
      json::Object io_obj;
      io_obj["installation_id"] = io.installation_id;
      io_obj["installation"] = installation_name(content, io.installation_id);
      io_obj["quantity_remaining"] = static_cast<double>(io.quantity_remaining);
      io_obj["minerals_paid"] = io.minerals_paid;
      io_obj["cp_remaining"] = io.cp_remaining;
      constr_q.emplace_back(std::move(io_obj));
    }
    obj["construction_queue"] = std::move(constr_q);

    // Ground battle (if active for this colony).
    if (auto itb = state.ground_battles.find(cid); itb != state.ground_battles.end()) {
      const GroundBattle& b = itb->second;
      json::Object bobj;
      bobj["attacker_faction_id"] = static_cast<double>(b.attacker_faction_id);
      bobj["attacker_faction"] = faction_name(state, b.attacker_faction_id);
      bobj["defender_faction_id"] = static_cast<double>(c.faction_id);
      bobj["defender_faction"] = faction_name(state, c.faction_id);
      bobj["system_id"] = static_cast<double>(b.system_id);
      bobj["system"] = system_name(state, b.system_id);
      bobj["attacker_strength"] = b.attacker_strength;
      bobj["defender_strength"] = b.defender_strength;
      bobj["days_fought"] = static_cast<double>(b.days_fought);
      obj["ground_battle"] = std::move(bobj);
    }

    out.emplace_back(std::move(obj));
  }

  std::string json_text = json::stringify(json::array(std::move(out)), 2);
  json_text.push_back('\n');
  return json_text;
}

std::string fleets_to_json(const GameState& state) {
  json::Array out;
  out.reserve(state.fleets.size());

  for (Id fid : sorted_keys(state.fleets)) {
    const auto it = state.fleets.find(fid);
    if (it == state.fleets.end()) continue;
    const Fleet& fl = it->second;

    json::Object obj;
    obj["id"] = static_cast<double>(fid);
    obj["name"] = fl.name;
    obj["faction_id"] = static_cast<double>(fl.faction_id);
    obj["faction"] = faction_name(state, fl.faction_id);

    obj["leader_ship_id"] = static_cast<double>(fl.leader_ship_id);
    if (const auto* leader = find_ptr(state.ships, fl.leader_ship_id)) {
      obj["leader_ship_name"] = leader->name;
      obj["leader_system_id"] = static_cast<double>(leader->system_id);
      obj["leader_system"] = system_name(state, leader->system_id);
    } else {
      obj["leader_ship_name"] = std::string{};
      obj["leader_system_id"] = static_cast<double>(kInvalidId);
      obj["leader_system"] = std::string{};
    }

    // Ship id list (numeric) + embedded per-ship summary.
    std::vector<Id> ship_ids = fl.ship_ids;
    std::sort(ship_ids.begin(), ship_ids.end());
    ship_ids.erase(std::unique(ship_ids.begin(), ship_ids.end()), ship_ids.end());

    json::Array ship_id_arr;
    ship_id_arr.reserve(ship_ids.size());
    for (Id sid : ship_ids) ship_id_arr.emplace_back(static_cast<double>(sid));
    obj["ship_ids"] = std::move(ship_id_arr);

    json::Array ships;
    ships.reserve(ship_ids.size());
    for (Id sid : ship_ids) {
      const auto* sh = find_ptr(state.ships, sid);
      if (!sh) continue;

      json::Object s;
      s["id"] = static_cast<double>(sid);
      s["name"] = sh->name;
      s["design_id"] = sh->design_id;
      s["system_id"] = static_cast<double>(sh->system_id);
      s["system"] = system_name(state, sh->system_id);
      s["position_mkm"] = vec2_to_object(sh->position_mkm);
      s["speed_km_s"] = sh->speed_km_s;
      s["hp"] = sh->hp;
      ships.emplace_back(std::move(s));
    }
    obj["ships"] = std::move(ships);

    out.emplace_back(std::move(obj));
  }

  std::string json_text = json::stringify(json::array(std::move(out)), 2);
  json_text.push_back('\n');
  return json_text;
}

std::string bodies_to_json(const GameState& state) {
  json::Array out;
  out.reserve(state.bodies.size());

  for (Id bid : sorted_keys(state.bodies)) {
    const auto it = state.bodies.find(bid);
    if (it == state.bodies.end()) continue;
    const Body& b = it->second;

    json::Object obj;
    obj["id"] = static_cast<double>(bid);
    obj["name"] = b.name;
    obj["type"] = std::string(body_type_label(b.type));

    obj["system_id"] = static_cast<double>(b.system_id);
    obj["system"] = system_name(state, b.system_id);

    obj["orbit_radius_mkm"] = b.orbit_radius_mkm;
    obj["orbit_period_days"] = b.orbit_period_days;
    obj["orbit_phase_radians"] = b.orbit_phase_radians;
    obj["position_mkm"] = vec2_to_object(b.position_mkm);

    if (b.parent_body_id != kInvalidId) {
      obj["parent_body_id"] = static_cast<double>(b.parent_body_id);
      obj["parent_body"] = body_name(state, b.parent_body_id);
    }

    if (b.mass_solar > 0.0) obj["mass_solar"] = b.mass_solar;
    if (b.luminosity_solar > 0.0) obj["luminosity_solar"] = b.luminosity_solar;
    if (b.mass_earths > 0.0) obj["mass_earths"] = b.mass_earths;
    if (b.radius_km > 0.0) obj["radius_km"] = b.radius_km;
    if (b.surface_temp_k > 0.0) obj["surface_temp_k"] = b.surface_temp_k;
    if (b.atmosphere_atm > 0.0) obj["atmosphere_atm"] = b.atmosphere_atm;
    if (b.terraforming_target_temp_k > 0.0) obj["terraforming_target_temp_k"] = b.terraforming_target_temp_k;
    if (b.terraforming_target_atm > 0.0) obj["terraforming_target_atm"] = b.terraforming_target_atm;
    if (b.terraforming_complete) obj["terraforming_complete"] = true;

    if (!b.mineral_deposits.empty()) {
      json::Object dep_obj;
      for (const auto& mineral : sorted_keys(b.mineral_deposits)) {
        dep_obj[mineral] = b.mineral_deposits.at(mineral);
      }
      obj["mineral_deposits"] = std::move(dep_obj);
      obj["mineral_deposits_total_tons"] = sum_positive(b.mineral_deposits);
    }

    out.emplace_back(std::move(obj));
  }

  std::string json_text = json::stringify(json::array(std::move(out)), 2);
  json_text.push_back('\n');
  return json_text;
}

} // namespace nebula4x
