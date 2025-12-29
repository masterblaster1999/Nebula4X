#include "nebula4x/core/serialization.h"

#include <algorithm>
#include <stdexcept>
#include <type_traits>

#include "nebula4x/util/json.h"
#include "nebula4x/util/log.h"

namespace nebula4x {
namespace {

using json::Array;
using json::Object;
using json::Value;

constexpr int kCurrentSaveVersion = 21;

template <typename Map>
std::vector<typename Map::key_type> sorted_keys(const Map& m) {
  std::vector<typename Map::key_type> keys;
  keys.reserve(m.size());
  for (const auto& [k, _] : m) keys.push_back(k);
  std::sort(keys.begin(), keys.end());
  return keys;
}

template <typename T>
std::vector<T> sorted_unique_copy(const std::vector<T>& v) {
  std::vector<T> out = v;
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

Value vec2_to_json(const Vec2& v) {
  Object o;
  o["x"] = v.x;
  o["y"] = v.y;
  return o;
}

Vec2 vec2_from_json(const Value& v) {
  const auto& o = v.object();
  return Vec2{o.at("x").number_value(), o.at("y").number_value()};
}

std::string body_type_to_string(BodyType t) {
  switch (t) {
    case BodyType::Star: return "star";
    case BodyType::Planet: return "planet";
    case BodyType::Moon: return "moon";
    case BodyType::Asteroid: return "asteroid";
    case BodyType::GasGiant: return "gas_giant";
  }
  return "planet";
}

BodyType body_type_from_string(const std::string& s) {
  if (s == "star") return BodyType::Star;
  if (s == "planet") return BodyType::Planet;
  if (s == "moon") return BodyType::Moon;
  if (s == "asteroid") return BodyType::Asteroid;
  if (s == "gas_giant") return BodyType::GasGiant;
  return BodyType::Planet;
}

std::string ship_role_to_string(ShipRole r) {
  switch (r) {
    case ShipRole::Freighter: return "freighter";
    case ShipRole::Surveyor: return "surveyor";
    case ShipRole::Combatant: return "combatant";
    default: return "unknown";
  }
}

ShipRole ship_role_from_string(const std::string& s) {
  if (s == "freighter") return ShipRole::Freighter;
  if (s == "surveyor") return ShipRole::Surveyor;
  if (s == "combatant") return ShipRole::Combatant;
  return ShipRole::Unknown;
}


std::string faction_control_to_string(FactionControl c) {
  switch (c) {
    case FactionControl::Player: return "player";
    case FactionControl::AI_Passive: return "ai_passive";
    case FactionControl::AI_Explorer: return "ai_explorer";
    case FactionControl::AI_Pirate: return "ai_pirate";
  }
  return "player";
}

FactionControl faction_control_from_string(const std::string& s) {
  if (s == "ai_passive" || s == "passive") return FactionControl::AI_Passive;
  if (s == "ai_explorer" || s == "explorer") return FactionControl::AI_Explorer;
  if (s == "ai_pirate" || s == "pirate") return FactionControl::AI_Pirate;
  return FactionControl::Player;
}

std::string diplomacy_status_to_string(DiplomacyStatus s) {
  switch (s) {
    case DiplomacyStatus::Friendly: return "friendly";
    case DiplomacyStatus::Neutral: return "neutral";
    case DiplomacyStatus::Hostile: return "hostile";
  }
  return "hostile";
}

DiplomacyStatus diplomacy_status_from_string(const std::string& s) {
  if (s == "friendly" || s == "friend" || s == "ally" || s == "allied") return DiplomacyStatus::Friendly;
  if (s == "neutral" || s == "non_hostile") return DiplomacyStatus::Neutral;
  if (s == "hostile" || s == "enemy") return DiplomacyStatus::Hostile;
  // Backward compat / safe default: unknown strings are treated as hostile.
  return DiplomacyStatus::Hostile;
}

const char* fleet_formation_to_string(FleetFormation f) {
  switch (f) {
    case FleetFormation::None: return "none";
    case FleetFormation::LineAbreast: return "line";
    case FleetFormation::Column: return "column";
    case FleetFormation::Wedge: return "wedge";
    case FleetFormation::Ring: return "ring";
  }
  return "none";
}

FleetFormation fleet_formation_from_string(const std::string& s) {
  if (s == "line") return FleetFormation::LineAbreast;
  if (s == "column") return FleetFormation::Column;
  if (s == "wedge") return FleetFormation::Wedge;
  if (s == "ring") return FleetFormation::Ring;
  return FleetFormation::None;
}

const char* event_level_to_string(EventLevel l) {
  switch (l) {
    case EventLevel::Info: return "info";
    case EventLevel::Warn: return "warn";
    case EventLevel::Error: return "error";
  }
  return "info";
}

const char* event_category_to_string(EventCategory c) {
  switch (c) {
    case EventCategory::General: return "general";
    case EventCategory::Research: return "research";
    case EventCategory::Shipyard: return "shipyard";
    case EventCategory::Construction: return "construction";
    case EventCategory::Movement: return "movement";
    case EventCategory::Combat: return "combat";
    case EventCategory::Intel: return "intel";
    case EventCategory::Exploration: return "exploration";
  }
  return "general";
}

EventCategory event_category_from_string(const std::string& s) {
  if (s == "research") return EventCategory::Research;
  if (s == "shipyard") return EventCategory::Shipyard;
  if (s == "construction") return EventCategory::Construction;
  if (s == "movement") return EventCategory::Movement;
  if (s == "combat") return EventCategory::Combat;
  if (s == "intel") return EventCategory::Intel;
  if (s == "exploration") return EventCategory::Exploration;
  return EventCategory::General;
}

EventLevel event_level_from_string(const std::string& s) {
  if (s == "warn") return EventLevel::Warn;
  if (s == "error") return EventLevel::Error;
  return EventLevel::Info;
}

Value order_to_json(const Order& order) {
  return std::visit(
      [](const auto& o) -> Value {
        using T = std::decay_t<decltype(o)>;
        Object obj;
        if constexpr (std::is_same_v<T, MoveToPoint>) {
          obj["type"] = std::string("move_to_point");
          obj["target"] = vec2_to_json(o.target_mkm);
        } else if constexpr (std::is_same_v<T, MoveToBody>) {
          obj["type"] = std::string("move_to_body");
          obj["body_id"] = static_cast<double>(o.body_id);
        } else if constexpr (std::is_same_v<T, ColonizeBody>) {
          obj["type"] = std::string("colonize_body");
          obj["body_id"] = static_cast<double>(o.body_id);
          if (!o.colony_name.empty()) obj["colony_name"] = o.colony_name;
        } else if constexpr (std::is_same_v<T, OrbitBody>) {
          obj["type"] = std::string("orbit_body");
          obj["body_id"] = static_cast<double>(o.body_id);
          obj["duration_days"] = static_cast<double>(o.duration_days);
        } else if constexpr (std::is_same_v<T, TravelViaJump>) {
          obj["type"] = std::string("travel_via_jump");
          obj["jump_point_id"] = static_cast<double>(o.jump_point_id);
        } else if constexpr (std::is_same_v<T, AttackShip>) {
          obj["type"] = std::string("attack_ship");
          obj["target_ship_id"] = static_cast<double>(o.target_ship_id);
          if (o.has_last_known) {
            obj["has_last_known"] = true;
            obj["last_known_position_mkm"] = vec2_to_json(o.last_known_position_mkm);
          }
        } else if constexpr (std::is_same_v<T, WaitDays>) {
          obj["type"] = std::string("wait_days");
          obj["days_remaining"] = static_cast<double>(o.days_remaining);
        } else if constexpr (std::is_same_v<T, LoadMineral>) {
          obj["type"] = std::string("load_mineral");
          obj["colony_id"] = static_cast<double>(o.colony_id);
          if (!o.mineral.empty()) obj["mineral"] = o.mineral;
          obj["tons"] = o.tons;
        } else if constexpr (std::is_same_v<T, UnloadMineral>) {
          obj["type"] = std::string("unload_mineral");
          obj["colony_id"] = static_cast<double>(o.colony_id);
          if (!o.mineral.empty()) obj["mineral"] = o.mineral;
          obj["tons"] = o.tons;
        } else if constexpr (std::is_same_v<T, TransferCargoToShip>) {
          obj["type"] = std::string("transfer_cargo_to_ship");
          obj["target_ship_id"] = static_cast<double>(o.target_ship_id);
          if (!o.mineral.empty()) obj["mineral"] = o.mineral;
          obj["tons"] = o.tons;
        } else if constexpr (std::is_same_v<T, ScrapShip>) {
          obj["type"] = std::string("scrap_ship");
          obj["colony_id"] = static_cast<double>(o.colony_id);
        }
        return obj;
      },
      order);
}

Order order_from_json(const Value& v) {
  const auto& o = v.object();
  const std::string type = o.at("type").string_value();
  if (type == "move_to_point") {
    MoveToPoint m;
    m.target_mkm = vec2_from_json(o.at("target"));
    return m;
  }
  if (type == "move_to_body") {
    MoveToBody m;
    m.body_id = static_cast<Id>(o.at("body_id").int_value());
    return m;
  }
  if (type == "colonize_body") {
    ColonizeBody c;
    c.body_id = static_cast<Id>(o.at("body_id").int_value());
    if (auto itn = o.find("colony_name"); itn != o.end()) c.colony_name = itn->second.string_value();
    return c;
  }
  if (type == "orbit_body") {
    OrbitBody m;
    m.body_id = static_cast<Id>(o.at("body_id").int_value());
    m.duration_days = static_cast<int>(o.at("duration_days").int_value(-1));
    return m;
  }
  if (type == "travel_via_jump") {
    TravelViaJump t;
    t.jump_point_id = static_cast<Id>(o.at("jump_point_id").int_value());
    return t;
  }
  if (type == "attack_ship") {
    AttackShip a;
    a.target_ship_id = static_cast<Id>(o.at("target_ship_id").int_value());

    if (auto lk_it = o.find("last_known_position_mkm"); lk_it != o.end()) {
      a.last_known_position_mkm = vec2_from_json(lk_it->second);
      a.has_last_known = true;
    }
    if (auto hk_it = o.find("has_last_known"); hk_it != o.end()) {
      a.has_last_known = hk_it->second.bool_value(a.has_last_known);
    }
    return a;
  }
  if (type == "wait_days") {
    WaitDays w;
    if (auto dr_it = o.find("days_remaining"); dr_it != o.end()) {
      w.days_remaining = static_cast<int>(dr_it->second.int_value(0));
    } else if (auto d_it = o.find("days"); d_it != o.end()) {
      w.days_remaining = static_cast<int>(d_it->second.int_value(0));
    }
    return w;
  }
  if (type == "load_mineral") {
    LoadMineral l;
    l.colony_id = static_cast<Id>(o.at("colony_id").int_value(kInvalidId));
    if (auto m_it = o.find("mineral"); m_it != o.end()) l.mineral = m_it->second.string_value();
    if (auto t_it = o.find("tons"); t_it != o.end()) l.tons = t_it->second.number_value(0.0);
    return l;
  }
  if (type == "unload_mineral") {
    UnloadMineral u;
    u.colony_id = static_cast<Id>(o.at("colony_id").int_value(kInvalidId));
    if (auto m_it = o.find("mineral"); m_it != o.end()) u.mineral = m_it->second.string_value();
    if (auto t_it = o.find("tons"); t_it != o.end()) u.tons = t_it->second.number_value(0.0);
    return u;
  }
  if (type == "transfer_cargo_to_ship") {
    TransferCargoToShip t;
    t.target_ship_id = static_cast<Id>(o.at("target_ship_id").int_value(kInvalidId));
    if (auto m_it = o.find("mineral"); m_it != o.end()) t.mineral = m_it->second.string_value();
    if (auto t_it = o.find("tons"); t_it != o.end()) t.tons = t_it->second.number_value(0.0);
    return t;
  }
  if (type == "scrap_ship") {
    ScrapShip s;
    s.colony_id = static_cast<Id>(o.at("colony_id").int_value(kInvalidId));
    return s;
  }
  throw std::runtime_error("Unknown order type: " + type);
}

Value map_string_double_to_json(const std::unordered_map<std::string, double>& m) {
  Object o;
  for (const auto& [k, v] : m) o[k] = v;
  return o;
}

std::unordered_map<std::string, double> map_string_double_from_json(const Value& v) {
  std::unordered_map<std::string, double> m;
  for (const auto& [k, val] : v.object()) m[k] = val.number_value();
  return m;
}

Value map_string_int_to_json(const std::unordered_map<std::string, int>& m) {
  Object o;
  for (const auto& [k, v] : m) o[k] = static_cast<double>(v);
  return o;
}

std::unordered_map<std::string, int> map_string_int_from_json(const Value& v) {
  std::unordered_map<std::string, int> m;
  for (const auto& [k, val] : v.object()) m[k] = static_cast<int>(val.int_value());
  return m;
}

Array string_vector_to_json(const std::vector<std::string>& v) {
  Array a;
  a.reserve(v.size());
  for (const auto& s : v) a.push_back(s);
  return a;
}

std::vector<std::string> string_vector_from_json(const Value& v) {
  std::vector<std::string> out;
  for (const auto& x : v.array()) out.push_back(x.string_value());
  return out;
}

} // namespace

std::string serialize_game_to_json(const GameState& s) {
  Object root;
  root["save_version"] = static_cast<double>(s.save_version);
  root["date"] = s.date.to_string();
  root["next_id"] = static_cast<double>(s.next_id);
  root["next_event_seq"] = static_cast<double>(s.next_event_seq);
  root["selected_system"] = static_cast<double>(s.selected_system);

  // Systems
  Array systems;
  systems.reserve(s.systems.size());
  for (Id id : sorted_keys(s.systems)) {
    const auto& sys = s.systems.at(id);
    Object o;
    o["id"] = static_cast<double>(id);
    o["name"] = sys.name;
    o["galaxy_pos"] = vec2_to_json(sys.galaxy_pos);

    Array bodies;
    {
      auto ids = sys.bodies;
      std::sort(ids.begin(), ids.end());
      for (Id bid : ids) bodies.push_back(static_cast<double>(bid));
    }
    o["bodies"] = bodies;

    Array ships;
    {
      auto ids = sys.ships;
      std::sort(ids.begin(), ids.end());
      for (Id sid : ids) ships.push_back(static_cast<double>(sid));
    }
    o["ships"] = ships;

    Array jps;
    {
      auto ids = sys.jump_points;
      std::sort(ids.begin(), ids.end());
      for (Id jid : ids) jps.push_back(static_cast<double>(jid));
    }
    o["jump_points"] = jps;

    systems.push_back(o);
  }
  root["systems"] = systems;

  // Bodies
  Array bodies;
  bodies.reserve(s.bodies.size());
  for (Id id : sorted_keys(s.bodies)) {
    const auto& b = s.bodies.at(id);
    Object o;
    o["id"] = static_cast<double>(id);
    o["name"] = b.name;
    o["type"] = body_type_to_string(b.type);
    o["system_id"] = static_cast<double>(b.system_id);
    o["orbit_radius_mkm"] = b.orbit_radius_mkm;
    o["orbit_period_days"] = b.orbit_period_days;
    o["orbit_phase_radians"] = b.orbit_phase_radians;
    bodies.push_back(o);
  }
  root["bodies"] = bodies;

  // Jump points
  Array jump_points;
  jump_points.reserve(s.jump_points.size());
  for (Id id : sorted_keys(s.jump_points)) {
    const auto& jp = s.jump_points.at(id);
    Object o;
    o["id"] = static_cast<double>(id);
    o["name"] = jp.name;
    o["system_id"] = static_cast<double>(jp.system_id);
    o["position_mkm"] = vec2_to_json(jp.position_mkm);
    o["linked_jump_id"] = static_cast<double>(jp.linked_jump_id);
    jump_points.push_back(o);
  }
  root["jump_points"] = jump_points;

  // Ships
  Array ships;
  ships.reserve(s.ships.size());
  for (Id id : sorted_keys(s.ships)) {
    const auto& sh = s.ships.at(id);
    Object o;
    o["id"] = static_cast<double>(id);
    o["name"] = sh.name;
    o["faction_id"] = static_cast<double>(sh.faction_id);
    o["system_id"] = static_cast<double>(sh.system_id);
    o["position_mkm"] = vec2_to_json(sh.position_mkm);
    o["design_id"] = sh.design_id;
    o["auto_explore"] = sh.auto_explore;
    o["auto_freight"] = sh.auto_freight;
    o["speed_km_s"] = sh.speed_km_s;
    o["hp"] = sh.hp;
    o["shields"] = sh.shields;
    o["cargo"] = map_string_double_to_json(sh.cargo);
    ships.push_back(o);
  }
  root["ships"] = ships;

  // Colonies
  Array colonies;
  colonies.reserve(s.colonies.size());
  for (Id id : sorted_keys(s.colonies)) {
    const auto& c = s.colonies.at(id);
    Object o;
    o["id"] = static_cast<double>(id);
    o["name"] = c.name;
    o["faction_id"] = static_cast<double>(c.faction_id);
    o["body_id"] = static_cast<double>(c.body_id);
    o["population_millions"] = c.population_millions;
    o["minerals"] = map_string_double_to_json(c.minerals);
    o["installations"] = map_string_int_to_json(c.installations);

    Array q;
    for (const auto& bo : c.shipyard_queue) {
      Object qo;
      qo["design_id"] = bo.design_id;
      qo["tons_remaining"] = bo.tons_remaining;
      if (bo.refit_ship_id != kInvalidId) {
        qo["refit_ship_id"] = static_cast<double>(bo.refit_ship_id);
      }
      q.push_back(qo);
    }
    o["shipyard_queue"] = q;

    Array cq;
    for (const auto& ord : c.construction_queue) {
      Object qo;
      qo["installation_id"] = ord.installation_id;
      qo["quantity_remaining"] = static_cast<double>(ord.quantity_remaining);
      qo["minerals_paid"] = ord.minerals_paid;
      qo["cp_remaining"] = ord.cp_remaining;
      cq.push_back(qo);
    }
    o["construction_queue"] = cq;

    colonies.push_back(o);
  }
  root["colonies"] = colonies;

  // Factions
  Array factions;
  factions.reserve(s.factions.size());
  for (Id id : sorted_keys(s.factions)) {
    const auto& f = s.factions.at(id);
    Object o;
    o["id"] = static_cast<double>(id);
    o["name"] = f.name;
    o["control"] = faction_control_to_string(f.control);
    o["research_points"] = f.research_points;
    o["active_research_id"] = f.active_research_id;
    o["active_research_progress"] = f.active_research_progress;
    o["research_queue"] = string_vector_to_json(f.research_queue);
    o["known_techs"] = string_vector_to_json(sorted_unique_copy(f.known_techs));
    o["unlocked_components"] = string_vector_to_json(sorted_unique_copy(f.unlocked_components));
    o["unlocked_installations"] = string_vector_to_json(sorted_unique_copy(f.unlocked_installations));

    // Diplomatic relations (optional; omitted when empty or default-hostile).
    if (!f.relations.empty()) {
      Object rel;
      for (const auto& [other_id, status] : f.relations) {
        if (other_id == kInvalidId || other_id == f.id) continue;
        // Hostile is the implicit default; don't write it out to keep saves tidy.
        if (status == DiplomacyStatus::Hostile) continue;
        rel[std::to_string(static_cast<unsigned long long>(other_id))] = diplomacy_status_to_string(status);
      }
      if (!rel.empty()) o["relations"] = rel;
    }


    Array discovered_systems;
    const auto disc = sorted_unique_copy(f.discovered_systems);
    discovered_systems.reserve(disc.size());
    for (Id sid : disc) discovered_systems.push_back(static_cast<double>(sid));
    o["discovered_systems"] = discovered_systems;

    Array contacts;
    contacts.reserve(f.ship_contacts.size());
    for (Id sid : sorted_keys(f.ship_contacts)) {
      const auto& c = f.ship_contacts.at(sid);
      Object co;
      co["ship_id"] = static_cast<double>(c.ship_id);
      co["system_id"] = static_cast<double>(c.system_id);
      co["last_seen_day"] = static_cast<double>(c.last_seen_day);
      co["last_seen_position_mkm"] = vec2_to_json(c.last_seen_position_mkm);
      co["last_seen_name"] = c.last_seen_name;
      co["last_seen_design_id"] = c.last_seen_design_id;
      co["last_seen_faction_id"] = static_cast<double>(c.last_seen_faction_id);
      contacts.push_back(co);
    }
    o["ship_contacts"] = contacts;
    factions.push_back(o);
  }
  root["factions"] = factions;

  // Fleets
  Array fleets;
  fleets.reserve(s.fleets.size());
  for (Id id : sorted_keys(s.fleets)) {
    const auto& f = s.fleets.at(id);
    Object o;
    o["id"] = static_cast<double>(id);
    o["name"] = f.name;
    o["faction_id"] = static_cast<double>(f.faction_id);
    o["leader_ship_id"] = static_cast<double>(f.leader_ship_id);

    o["formation"] = std::string(fleet_formation_to_string(f.formation));
    o["formation_spacing_mkm"] = f.formation_spacing_mkm;

    auto fleet_ship_ids = f.ship_ids;
    std::sort(fleet_ship_ids.begin(), fleet_ship_ids.end());
    fleet_ship_ids.erase(std::unique(fleet_ship_ids.begin(), fleet_ship_ids.end()), fleet_ship_ids.end());
    Array ship_ids;
    ship_ids.reserve(fleet_ship_ids.size());
    for (Id sid : fleet_ship_ids) ship_ids.push_back(static_cast<double>(sid));
    o["ship_ids"] = ship_ids;

    fleets.push_back(o);
  }
  root["fleets"] = fleets;

  // Custom designs
  Array designs;
  designs.reserve(s.custom_designs.size());
  for (const auto& id : sorted_keys(s.custom_designs)) {
    const auto& d = s.custom_designs.at(id);
    Object o;
    o["id"] = d.id;
    o["name"] = d.name;
    o["role"] = ship_role_to_string(d.role);
    o["components"] = string_vector_to_json(d.components);
    o["mass_tons"] = d.mass_tons;
    o["speed_km_s"] = d.speed_km_s;
    o["cargo_tons"] = d.cargo_tons;
    o["sensor_range_mkm"] = d.sensor_range_mkm;
    o["colony_capacity_millions"] = d.colony_capacity_millions;
    o["max_hp"] = d.max_hp;
    o["max_shields"] = d.max_shields;
    o["shield_regen_per_day"] = d.shield_regen_per_day;
    o["weapon_damage"] = d.weapon_damage;
    o["weapon_range_mkm"] = d.weapon_range_mkm;
    designs.push_back(o);
  }
  root["custom_designs"] = designs;

  // Player order templates
  Array templates;
  templates.reserve(s.order_templates.size());
  for (const auto& name : sorted_keys(s.order_templates)) {
    const auto& orders = s.order_templates.at(name);
    Object o;
    o["name"] = name;

    Array q;
    q.reserve(orders.size());
    for (const auto& ord : orders) q.push_back(order_to_json(ord));
    o["orders"] = q;

    templates.push_back(o);
  }
  root["order_templates"] = templates;

  // Orders
  Array ship_orders;
  ship_orders.reserve(s.ship_orders.size());
  for (Id ship_id : sorted_keys(s.ship_orders)) {
    const auto& orders = s.ship_orders.at(ship_id);
    Object o;
    o["ship_id"] = static_cast<double>(ship_id);
    Array q;
    for (const auto& ord : orders.queue) q.push_back(order_to_json(ord));
    o["queue"] = q;

    o["repeat"] = orders.repeat;
    Array tmpl;
    tmpl.reserve(orders.repeat_template.size());
    for (const auto& ord : orders.repeat_template) tmpl.push_back(order_to_json(ord));
    o["repeat_template"] = tmpl;

    ship_orders.push_back(o);
  }
  root["ship_orders"] = ship_orders;

  // Persistent simulation event log.
  Array events;
  events.reserve(s.events.size());
  for (const auto& ev : s.events) {
    Object o;
    o["seq"] = static_cast<double>(ev.seq);
    o["day"] = static_cast<double>(ev.day);
    o["level"] = std::string(event_level_to_string(ev.level));
    o["category"] = std::string(event_category_to_string(ev.category));
    o["faction_id"] = static_cast<double>(ev.faction_id);
    o["faction_id2"] = static_cast<double>(ev.faction_id2);
    o["system_id"] = static_cast<double>(ev.system_id);
    o["ship_id"] = static_cast<double>(ev.ship_id);
    o["colony_id"] = static_cast<double>(ev.colony_id);
    o["message"] = ev.message;
    events.push_back(o);
  }
  root["events"] = events;

  return json::stringify(root, 2);
}

GameState deserialize_game_from_json(const std::string& json_text) {
  const auto root = json::parse(json_text).object();

  GameState s;
  {
    int loaded_version = 1;
    if (auto itv = root.find("save_version"); itv != root.end()) {
      loaded_version = static_cast<int>(itv->second.int_value(1));
    }
    s.save_version = loaded_version < kCurrentSaveVersion ? kCurrentSaveVersion : loaded_version;
  }

  s.date = Date::parse_iso_ymd(root.at("date").string_value());

  s.next_id = 1;
  if (auto itnid = root.find("next_id"); itnid != root.end()) {
    s.next_id = static_cast<Id>(itnid->second.int_value(1));
  }

  s.next_event_seq = 1;
  if (auto itseq = root.find("next_event_seq"); itseq != root.end()) {
    s.next_event_seq = static_cast<std::uint64_t>(itseq->second.int_value(1));
  }
  if (s.next_event_seq == 0) s.next_event_seq = 1;

  s.selected_system = kInvalidId;
  if (auto itsel = root.find("selected_system"); itsel != root.end()) {
    s.selected_system = static_cast<Id>(itsel->second.int_value(kInvalidId));
  }

  // Systems
  for (const auto& sv : root.at("systems").array()) {
    const auto& o = sv.object();
    StarSystem sys;
    sys.id = static_cast<Id>(o.at("id").int_value());
    sys.name = o.at("name").string_value();
    sys.galaxy_pos = vec2_from_json(o.at("galaxy_pos"));

    for (const auto& bid : o.at("bodies").array()) sys.bodies.push_back(static_cast<Id>(bid.int_value()));
    for (const auto& sid : o.at("ships").array()) sys.ships.push_back(static_cast<Id>(sid.int_value()));

    if (auto it = o.find("jump_points"); it != o.end()) {
      for (const auto& jid : it->second.array()) sys.jump_points.push_back(static_cast<Id>(jid.int_value()));
    }

    s.systems[sys.id] = sys;
  }

  if (s.selected_system != kInvalidId && s.systems.find(s.selected_system) == s.systems.end()) {
    s.selected_system = kInvalidId;
  }

  // Bodies
  for (const auto& bv : root.at("bodies").array()) {
    const auto& o = bv.object();
    Body b;
    b.id = static_cast<Id>(o.at("id").int_value());
    b.name = o.at("name").string_value();
    b.type = body_type_from_string(o.at("type").string_value());
    b.system_id = static_cast<Id>(o.at("system_id").int_value());
    b.orbit_radius_mkm = o.at("orbit_radius_mkm").number_value();
    b.orbit_period_days = o.at("orbit_period_days").number_value();
    b.orbit_phase_radians = o.at("orbit_phase_radians").number_value();
    s.bodies[b.id] = b;
  }

  // Jump points
  if (auto it = root.find("jump_points"); it != root.end()) {
    for (const auto& jv : it->second.array()) {
      const auto& o = jv.object();
      JumpPoint jp;
      jp.id = static_cast<Id>(o.at("id").int_value());
      jp.name = o.at("name").string_value();
      jp.system_id = static_cast<Id>(o.at("system_id").int_value());
      jp.position_mkm = vec2_from_json(o.at("position_mkm"));
      jp.linked_jump_id = static_cast<Id>(o.at("linked_jump_id").int_value(kInvalidId));
      s.jump_points[jp.id] = jp;
    }
  }

  // Ships
  for (const auto& shv : root.at("ships").array()) {
    const auto& o = shv.object();
    Ship sh;
    sh.id = static_cast<Id>(o.at("id").int_value());
    sh.name = o.at("name").string_value();
    sh.faction_id = static_cast<Id>(o.at("faction_id").int_value());
    sh.system_id = static_cast<Id>(o.at("system_id").int_value());
    sh.position_mkm = vec2_from_json(o.at("position_mkm"));
    sh.design_id = o.at("design_id").string_value();
    if (auto it = o.find("auto_explore"); it != o.end()) sh.auto_explore = it->second.bool_value(false);
    if (auto it = o.find("auto_freight"); it != o.end()) sh.auto_freight = it->second.bool_value(false);
    sh.speed_km_s = o.at("speed_km_s").number_value(0.0);
    sh.hp = o.at("hp").number_value(0.0);
    if (auto it = o.find("shields"); it != o.end()) sh.shields = it->second.number_value(-1.0);
    if (auto it = o.find("cargo"); it != o.end()) sh.cargo = map_string_double_from_json(it->second);
    s.ships[sh.id] = sh;
  }

  // Colonies
  for (const auto& cv : root.at("colonies").array()) {
    const auto& o = cv.object();
    Colony c;
    c.id = static_cast<Id>(o.at("id").int_value());
    c.name = o.at("name").string_value();
    c.faction_id = static_cast<Id>(o.at("faction_id").int_value());
    c.body_id = static_cast<Id>(o.at("body_id").int_value());
    c.population_millions = o.at("population_millions").number_value();
    c.minerals = map_string_double_from_json(o.at("minerals"));
    c.installations = map_string_int_from_json(o.at("installations"));

    if (auto itq = o.find("shipyard_queue"); itq != o.end()) {
      for (const auto& qv : itq->second.array()) {
        const auto& qo = qv.object();
        BuildOrder bo;
        bo.design_id = qo.at("design_id").string_value();
        bo.tons_remaining = qo.at("tons_remaining").number_value();
        if (auto it = qo.find("refit_ship_id"); it != qo.end()) {
          bo.refit_ship_id = static_cast<Id>(it->second.int_value(kInvalidId));
        }
        c.shipyard_queue.push_back(bo);
      }
    }

    if (auto itq = o.find("construction_queue"); itq != o.end()) {
      for (const auto& qv : itq->second.array()) {
        const auto& qo = qv.object();
        InstallationBuildOrder ord;
        ord.installation_id = qo.at("installation_id").string_value();
        ord.quantity_remaining = static_cast<int>(qo.at("quantity_remaining").int_value(0));
        if (auto it = qo.find("minerals_paid"); it != qo.end()) ord.minerals_paid = it->second.bool_value(false);
        if (auto it = qo.find("cp_remaining"); it != qo.end()) ord.cp_remaining = it->second.number_value(0.0);
        c.construction_queue.push_back(ord);
      }
    }

    s.colonies[c.id] = c;
  }

  // Factions
  for (const auto& fv : root.at("factions").array()) {
    const auto& o = fv.object();
    Faction f;
    f.id = static_cast<Id>(o.at("id").int_value());
    f.name = o.at("name").string_value();
    if (auto it = o.find("control"); it != o.end()) {
      f.control = faction_control_from_string(it->second.string_value("player"));
    }
    f.research_points = o.at("research_points").number_value(0.0);

    if (auto it = o.find("active_research_id"); it != o.end()) f.active_research_id = it->second.string_value();
    if (auto it = o.find("active_research_progress"); it != o.end()) f.active_research_progress = it->second.number_value(0.0);

    if (auto it = o.find("research_queue"); it != o.end()) f.research_queue = string_vector_from_json(it->second);
    if (auto it = o.find("known_techs"); it != o.end()) f.known_techs = string_vector_from_json(it->second);

    if (auto it = o.find("unlocked_components"); it != o.end()) f.unlocked_components = string_vector_from_json(it->second);
    if (auto it = o.find("unlocked_installations"); it != o.end()) f.unlocked_installations = string_vector_from_json(it->second);

    // Diplomatic relations (optional in saves; if missing, default stance is Hostile).
    if (auto it = o.find("relations"); it != o.end()) {
      if (it->second.is_object()) {
        for (const auto& [k, v] : it->second.object()) {
          Id other = kInvalidId;
          try {
            other = static_cast<Id>(std::stoull(k));
          } catch (...) {
            continue;
          }
          if (other == kInvalidId || other == f.id) continue;
          const DiplomacyStatus ds = diplomacy_status_from_string(v.string_value("hostile"));
          if (ds == DiplomacyStatus::Hostile) continue;
          f.relations[other] = ds;
        }
      } else if (it->second.is_array()) {
        // Also accept an array-of-objects format:
        //   [{"faction_id": 2, "status": "neutral"}, ...]
        for (const auto& rv : it->second.array()) {
          if (!rv.is_object()) continue;
          const auto& ro = rv.object();
          auto itid = ro.find("faction_id");
          if (itid == ro.end()) continue;
          const Id other = static_cast<Id>(itid->second.int_value(kInvalidId));
          if (other == kInvalidId || other == f.id) continue;
          const std::string st = (ro.find("status") != ro.end()) ? ro.at("status").string_value("hostile") : std::string("hostile");
          const DiplomacyStatus ds = diplomacy_status_from_string(st);
          if (ds == DiplomacyStatus::Hostile) continue;
          f.relations[other] = ds;
        }
      }
    }


    if (auto it = o.find("discovered_systems"); it != o.end()) {
      for (const auto& sv : it->second.array()) {
        f.discovered_systems.push_back(static_cast<Id>(sv.int_value(kInvalidId)));
      }
    }

    if (auto it = o.find("ship_contacts"); it != o.end()) {
      for (const auto& cv : it->second.array()) {
        const auto& co = cv.object();
        Contact c;
        c.ship_id = static_cast<Id>(co.at("ship_id").int_value());
        c.system_id = static_cast<Id>(co.at("system_id").int_value(kInvalidId));
        c.last_seen_day = static_cast<int>(co.at("last_seen_day").int_value(0));
        if (auto itp = co.find("last_seen_position_mkm"); itp != co.end()) c.last_seen_position_mkm = vec2_from_json(itp->second);
        if (auto itn = co.find("last_seen_name"); itn != co.end()) c.last_seen_name = itn->second.string_value();
        if (auto itd = co.find("last_seen_design_id"); itd != co.end()) c.last_seen_design_id = itd->second.string_value();
        if (auto itf = co.find("last_seen_faction_id"); itf != co.end()) c.last_seen_faction_id = static_cast<Id>(itf->second.int_value(kInvalidId));
        if (c.ship_id != kInvalidId) f.ship_contacts[c.ship_id] = std::move(c);
      }
    }

    s.factions[f.id] = f;
  }

  // Fleets (optional field; older saves may not include it).
  if (auto it = root.find("fleets"); it != root.end()) {
    if (!it->second.is_array()) {
      log::warn("Save load: 'fleets' is not an array; ignoring");
    } else {
      for (const auto& fv : it->second.array()) {
        if (!fv.is_object()) continue;
        const auto& o = fv.object();

        Fleet fl;
        fl.id = static_cast<Id>(o.at("id").int_value(kInvalidId));
        if (fl.id == kInvalidId) continue;
        fl.name = o.at("name").string_value();
        fl.faction_id = static_cast<Id>(o.at("faction_id").int_value(kInvalidId));

        if (auto itl = o.find("leader_ship_id"); itl != o.end()) {
          fl.leader_ship_id = static_cast<Id>(itl->second.int_value(kInvalidId));
        }

        if (auto its = o.find("ship_ids"); its != o.end() && its->second.is_array()) {
          for (const auto& sv : its->second.array()) {
            fl.ship_ids.push_back(static_cast<Id>(sv.int_value(kInvalidId)));
          }
        }

        if (auto itf = o.find("formation"); itf != o.end()) {
          fl.formation = fleet_formation_from_string(itf->second.string_value("none"));
        }
        if (auto its = o.find("formation_spacing_mkm"); its != o.end()) {
          fl.formation_spacing_mkm = std::max(0.0, its->second.number_value(1.0));
        }

        s.fleets[fl.id] = std::move(fl);
      }
    }
  }

  // Custom designs
  if (auto it = root.find("custom_designs"); it != root.end()) {
    for (const auto& dv : it->second.array()) {
      const auto& o = dv.object();
      ShipDesign d;
      d.id = o.at("id").string_value();
      d.name = o.at("name").string_value();
      d.role = ship_role_from_string(o.at("role").string_value("unknown"));
      if (auto itc = o.find("components"); itc != o.end()) d.components = string_vector_from_json(itc->second);
      d.mass_tons = o.at("mass_tons").number_value(0.0);
      d.speed_km_s = o.at("speed_km_s").number_value(0.0);
      d.cargo_tons = o.at("cargo_tons").number_value(0.0);
      d.sensor_range_mkm = o.at("sensor_range_mkm").number_value(0.0);
      if (auto itcc = o.find("colony_capacity_millions"); itcc != o.end()) {
        d.colony_capacity_millions = itcc->second.number_value(0.0);
      }
      d.max_hp = o.at("max_hp").number_value(0.0);
      if (auto itms = o.find("max_shields"); itms != o.end()) d.max_shields = itms->second.number_value(0.0);
      if (auto itsr = o.find("shield_regen_per_day"); itsr != o.end()) {
        d.shield_regen_per_day = itsr->second.number_value(0.0);
      }
      d.weapon_damage = o.at("weapon_damage").number_value(0.0);
      d.weapon_range_mkm = o.at("weapon_range_mkm").number_value(0.0);
      s.custom_designs[d.id] = d;
    }
  }

  // Player order templates
  if (auto it = root.find("order_templates"); it != root.end()) {
    if (!it->second.is_array()) {
      log::warn("Save load: 'order_templates' is not an array; ignoring");
    } else {
      std::size_t dropped = 0;
      std::size_t detail_logs = 0;
      constexpr std::size_t kMaxDetailLogs = 8;

      for (const auto& tv : it->second.array()) {
        if (!tv.is_object()) {
          dropped += 1;
          continue;
        }

        const auto& o = tv.object();
        const std::string name = (o.find("name") != o.end()) ? o.at("name").string_value() : std::string();
        if (name.empty()) {
          dropped += 1;
          continue;
        }

        std::vector<Order> orders;
        if (auto ito = o.find("orders"); ito != o.end()) {
          if (!ito->second.is_array()) {
            dropped += 1;
          } else {
            for (const auto& qv : ito->second.array()) {
              try {
                orders.push_back(order_from_json(qv));
              } catch (const std::exception& e) {
                dropped += 1;
                if (detail_logs < kMaxDetailLogs) {
                  detail_logs += 1;
                  log::warn(std::string("Save load: dropped invalid order in template '") + name + "': " + e.what());
                }
              }
            }
          }
        }

        if (!orders.empty()) {
          s.order_templates[name] = std::move(orders);
        }
      }

      if (dropped > 0) {
        log::warn("Save load: dropped " + std::to_string(static_cast<unsigned long long>(dropped)) +
                  " invalid order template entries");
      }
    }
  }

  // Orders
  if (auto it = root.find("ship_orders"); it != root.end()) {
    if (!it->second.is_array()) {
      log::warn("Save load: 'ship_orders' is not an array; ignoring");
    } else {
      std::size_t dropped = 0;
      std::size_t detail_logs = 0;
      constexpr std::size_t kMaxDetailLogs = 8;

      for (const auto& ov : it->second.array()) {
        if (!ov.is_object()) {
          dropped += 1;
          continue;
        }

        const auto& o = ov.object();

        Id ship_id = kInvalidId;
        if (auto itid = o.find("ship_id"); itid != o.end()) {
          ship_id = static_cast<Id>(itid->second.int_value(kInvalidId));
        }
        if (ship_id == kInvalidId) {
          dropped += 1;
          continue;
        }

        ShipOrders so;

        auto parse_order_list = [&](const Value& list_v, std::vector<Order>& out, const char* label) {
          if (!list_v.is_array()) {
            dropped += 1;
            return;
          }
          for (const auto& qv : list_v.array()) {
            try {
              out.push_back(order_from_json(qv));
            } catch (const std::exception& e) {
              dropped += 1;
              if (detail_logs < kMaxDetailLogs) {
                detail_logs += 1;
                log::warn(std::string("Save load: dropped invalid order in '") + label +
                          "' for ship_id=" + std::to_string(static_cast<unsigned long long>(ship_id)) +
                          ": " + e.what());
              }
            }
          }
        };

        if (auto itq = o.find("queue"); itq != o.end()) {
          parse_order_list(itq->second, so.queue, "queue");
        }

        if (auto itrep = o.find("repeat"); itrep != o.end()) {
          so.repeat = itrep->second.bool_value(false);
        }
        if (auto ittmpl = o.find("repeat_template"); ittmpl != o.end()) {
          parse_order_list(ittmpl->second, so.repeat_template, "repeat_template");
        }

        s.ship_orders[ship_id] = std::move(so);
      }

      if (dropped > 0) {
        log::warn("Save load: dropped " + std::to_string(static_cast<unsigned long long>(dropped)) +
                  " invalid ship order entries");
      }
    }
  }

  // Persistent simulation event log.
  if (auto it = root.find("events"); it != root.end()) {
    std::uint64_t seq_cursor = 0;
    for (const auto& evv : it->second.array()) {
      const auto& o = evv.object();
      SimEvent ev;

      std::uint64_t wanted_seq = 0;
      if (auto its = o.find("seq"); its != o.end()) {
        wanted_seq = static_cast<std::uint64_t>(its->second.int_value(0));
      }
      if (wanted_seq == 0) wanted_seq = seq_cursor + 1;
      if (wanted_seq <= seq_cursor) wanted_seq = seq_cursor + 1;
      ev.seq = wanted_seq;
      seq_cursor = ev.seq;

      ev.day = static_cast<std::int64_t>(o.at("day").int_value(0));
      if (auto itl = o.find("level"); itl != o.end()) ev.level = event_level_from_string(itl->second.string_value("info"));
      if (auto itc = o.find("category"); itc != o.end()) {
        ev.category = event_category_from_string(itc->second.string_value("general"));
      }

      if (auto itf = o.find("faction_id"); itf != o.end()) ev.faction_id = static_cast<Id>(itf->second.int_value(kInvalidId));
      if (auto itf2 = o.find("faction_id2"); itf2 != o.end()) ev.faction_id2 = static_cast<Id>(itf2->second.int_value(kInvalidId));
      if (auto its = o.find("system_id"); its != o.end()) ev.system_id = static_cast<Id>(its->second.int_value(kInvalidId));
      if (auto itsh = o.find("ship_id"); itsh != o.end()) ev.ship_id = static_cast<Id>(itsh->second.int_value(kInvalidId));
      if (auto itcol = o.find("colony_id"); itcol != o.end()) ev.colony_id = static_cast<Id>(itcol->second.int_value(kInvalidId));
      if (auto itm = o.find("message"); itm != o.end()) ev.message = itm->second.string_value();
      s.events.push_back(std::move(ev));
    }

    if (s.next_event_seq <= seq_cursor) s.next_event_seq = seq_cursor + 1;
  }

  // Ensure next_id is sane.
  Id max_id = 0;
  auto bump = [&](Id id) { max_id = std::max(max_id, id); };
  for (auto& [id, _] : s.systems) bump(id);
  for (auto& [id, _] : s.bodies) bump(id);
  for (auto& [id, _] : s.jump_points) bump(id);
  for (auto& [id, _] : s.ships) bump(id);
  for (auto& [id, _] : s.colonies) bump(id);
  for (auto& [id, _] : s.factions) bump(id);
  for (auto& [id, _] : s.fleets) bump(id);
  if (s.next_id <= max_id) s.next_id = max_id + 1;

  return s;
}

} // namespace nebula4x
