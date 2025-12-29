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

    obj["design_mass_tons"] = d ? d->mass_tons : 0.0;
    obj["design_cargo_tons"] = d ? d->cargo_tons : 0.0;
    obj["design_sensor_range_mkm"] = d ? d->sensor_range_mkm : 0.0;
    obj["design_colony_capacity_millions"] = d ? d->colony_capacity_millions : 0.0;
    obj["design_max_hp"] = d ? d->max_hp : 0.0;
    obj["design_max_shields"] = d ? d->max_shields : 0.0;
    obj["design_shield_regen_per_day"] = d ? d->shield_regen_per_day : 0.0;
    obj["design_weapon_damage"] = d ? d->weapon_damage : 0.0;
    obj["design_weapon_range_mkm"] = d ? d->weapon_range_mkm : 0.0;

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
    double construction_points_per_day = 0.0;
    double shipyard_capacity_tons_per_day = 0.0;

    if (content) {
      for (const auto& [inst_id, count] : c.installations) {
        if (count <= 0) continue;
        const InstallationDef* def = find_installation(content, inst_id);
        if (!def) continue;

        for (const auto& [mineral, per_day] : def->produces_per_day) {
          prod[mineral] += per_day * static_cast<double>(count);
        }

        construction_points_per_day += def->construction_points_per_day * static_cast<double>(count);
        shipyard_capacity_tons_per_day += def->build_rate_tons_per_day * static_cast<double>(count);
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

    json::Object minerals;
    for (const auto& [k, v] : c.minerals) minerals[k] = v;
    obj["minerals"] = std::move(minerals);

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

    obj["construction_points_per_day"] = construction_points_per_day;
    obj["shipyard_capacity_tons_per_day"] = shipyard_capacity_tons_per_day;

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

} // namespace nebula4x
