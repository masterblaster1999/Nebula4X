#include "nebula4x/core/serialization.h"

#include <algorithm>
#include <stdexcept>
#include <type_traits>

#include "nebula4x/util/json.h"

namespace nebula4x {
namespace {

using json::Array;
using json::Object;
using json::Value;

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
  if (type == "travel_via_jump") {
    TravelViaJump t;
    t.jump_point_id = static_cast<Id>(o.at("jump_point_id").int_value());
    return t;
  }
  if (type == "attack_ship") {
    AttackShip a;
    a.target_ship_id = static_cast<Id>(o.at("target_ship_id").int_value());

    // Back-compat: older saves won't have last-known tracking.
    if (auto it = o.find("last_known_position_mkm"); it != o.end()) {
      a.last_known_position_mkm = vec2_from_json(it->second);
      a.has_last_known = true;
    }
    if (auto it = o.find("has_last_known"); it != o.end()) {
      a.has_last_known = it->second.bool_value(a.has_last_known);
    }

    return a;
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
  root["selected_system"] = static_cast<double>(s.selected_system);

  // Systems
  Array systems;
  systems.reserve(s.systems.size());
  for (const auto& [id, sys] : s.systems) {
    Object o;
    o["id"] = static_cast<double>(id);
    o["name"] = sys.name;
    o["galaxy_pos"] = vec2_to_json(sys.galaxy_pos);

    Array bodies;
    for (Id bid : sys.bodies) bodies.push_back(static_cast<double>(bid));
    o["bodies"] = bodies;

    Array ships;
    for (Id sid : sys.ships) ships.push_back(static_cast<double>(sid));
    o["ships"] = ships;

    Array jps;
    for (Id jid : sys.jump_points) jps.push_back(static_cast<double>(jid));
    o["jump_points"] = jps;

    systems.push_back(o);
  }
  root["systems"] = systems;

  // Bodies
  Array bodies;
  bodies.reserve(s.bodies.size());
  for (const auto& [id, b] : s.bodies) {
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
  for (const auto& [id, jp] : s.jump_points) {
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
  for (const auto& [id, sh] : s.ships) {
    Object o;
    o["id"] = static_cast<double>(id);
    o["name"] = sh.name;
    o["faction_id"] = static_cast<double>(sh.faction_id);
    o["system_id"] = static_cast<double>(sh.system_id);
    o["position_mkm"] = vec2_to_json(sh.position_mkm);
    o["design_id"] = sh.design_id;
    o["speed_km_s"] = sh.speed_km_s;
    o["hp"] = sh.hp;
    ships.push_back(o);
  }
  root["ships"] = ships;

  // Colonies
  Array colonies;
  colonies.reserve(s.colonies.size());
  for (const auto& [id, c] : s.colonies) {
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
  for (const auto& [id, f] : s.factions) {
    Object o;
    o["id"] = static_cast<double>(id);
    o["name"] = f.name;
    o["research_points"] = f.research_points;
    o["active_research_id"] = f.active_research_id;
    o["active_research_progress"] = f.active_research_progress;
    o["research_queue"] = string_vector_to_json(f.research_queue);
    o["known_techs"] = string_vector_to_json(f.known_techs);
    o["unlocked_components"] = string_vector_to_json(f.unlocked_components);
    o["unlocked_installations"] = string_vector_to_json(f.unlocked_installations);

    // Optional contact memory (prototype intel).
    Array contacts;
    contacts.reserve(f.ship_contacts.size());
    for (const auto& [_, c] : f.ship_contacts) {
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

  // Custom designs
  Array designs;
  designs.reserve(s.custom_designs.size());
  for (const auto& [id, d] : s.custom_designs) {
    Object o;
    o["id"] = d.id;
    o["name"] = d.name;
    o["role"] = ship_role_to_string(d.role);
    o["components"] = string_vector_to_json(d.components);
    o["mass_tons"] = d.mass_tons;
    o["speed_km_s"] = d.speed_km_s;
    o["cargo_tons"] = d.cargo_tons;
    o["sensor_range_mkm"] = d.sensor_range_mkm;
    o["max_hp"] = d.max_hp;
    o["weapon_damage"] = d.weapon_damage;
    o["weapon_range_mkm"] = d.weapon_range_mkm;
    designs.push_back(o);
  }
  root["custom_designs"] = designs;

  // Orders
  Array ship_orders;
  ship_orders.reserve(s.ship_orders.size());
  for (const auto& [ship_id, orders] : s.ship_orders) {
    Object o;
    o["ship_id"] = static_cast<double>(ship_id);
    Array q;
    for (const auto& ord : orders.queue) q.push_back(order_to_json(ord));
    o["queue"] = q;
    ship_orders.push_back(o);
  }
  root["ship_orders"] = ship_orders;

  return json::stringify(root, 2);
}

GameState deserialize_game_from_json(const std::string& json_text) {
  const auto root = json::parse(json_text).object();

  GameState s;
  s.save_version = static_cast<int>(root.at("save_version").int_value(1));
  s.date = Date::parse_iso_ymd(root.at("date").string_value());
  s.next_id = static_cast<Id>(root.at("next_id").int_value(1));
  s.selected_system = static_cast<Id>(root.at("selected_system").int_value(kInvalidId));

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
    sh.speed_km_s = o.at("speed_km_s").number_value(0.0);
    sh.hp = o.at("hp").number_value(0.0);
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

    // shipyard_queue was added after the earliest prototypes; treat it as optional.
    if (auto itq = o.find("shipyard_queue"); itq != o.end()) {
      for (const auto& qv : itq->second.array()) {
        const auto& qo = qv.object();
        BuildOrder bo;
        bo.design_id = qo.at("design_id").string_value();
        bo.tons_remaining = qo.at("tons_remaining").number_value();
        c.shipyard_queue.push_back(bo);
      }
    }

    // construction_queue is optional (older saves won't have it).
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
    f.research_points = o.at("research_points").number_value(0.0);

    if (auto it = o.find("active_research_id"); it != o.end()) f.active_research_id = it->second.string_value();
    if (auto it = o.find("active_research_progress"); it != o.end()) f.active_research_progress = it->second.number_value(0.0);

    if (auto it = o.find("research_queue"); it != o.end()) f.research_queue = string_vector_from_json(it->second);
    if (auto it = o.find("known_techs"); it != o.end()) f.known_techs = string_vector_from_json(it->second);

    if (auto it = o.find("unlocked_components"); it != o.end()) f.unlocked_components = string_vector_from_json(it->second);
    if (auto it = o.find("unlocked_installations"); it != o.end()) f.unlocked_installations = string_vector_from_json(it->second);

    // Optional contact memory.
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
      d.max_hp = o.at("max_hp").number_value(0.0);
      d.weapon_damage = o.at("weapon_damage").number_value(0.0);
      d.weapon_range_mkm = o.at("weapon_range_mkm").number_value(0.0);
      s.custom_designs[d.id] = d;
    }
  }

  // Orders
  if (auto it = root.find("ship_orders"); it != root.end()) {
    for (const auto& ov : it->second.array()) {
      const auto& o = ov.object();
      const Id ship_id = static_cast<Id>(o.at("ship_id").int_value());
      ShipOrders so;
      for (const auto& qv : o.at("queue").array()) so.queue.push_back(order_from_json(qv));
      s.ship_orders[ship_id] = so;
    }
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
  if (s.next_id <= max_id) s.next_id = max_id + 1;

  return s;
}

} // namespace nebula4x
