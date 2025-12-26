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

    Array techs;
    for (const auto& t : f.known_techs) techs.push_back(t);
    o["known_techs"] = techs;

    factions.push_back(o);
  }
  root["factions"] = factions;

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

    for (const auto& qv : o.at("shipyard_queue").array()) {
      const auto& qo = qv.object();
      BuildOrder bo;
      bo.design_id = qo.at("design_id").string_value();
      bo.tons_remaining = qo.at("tons_remaining").number_value();
      c.shipyard_queue.push_back(bo);
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
    for (const auto& tv : o.at("known_techs").array()) f.known_techs.push_back(tv.string_value());
    s.factions[f.id] = f;
  }

  // Orders
  for (const auto& ov : root.at("ship_orders").array()) {
    const auto& o = ov.object();
    const Id ship_id = static_cast<Id>(o.at("ship_id").int_value());
    ShipOrders so;
    for (const auto& qv : o.at("queue").array()) so.queue.push_back(order_from_json(qv));
    s.ship_orders[ship_id] = so;
  }

  // Ensure next_id is sane.
  Id max_id = 0;
  auto bump = [&](Id id) { max_id = std::max(max_id, id); };
  for (auto& [id, _] : s.systems) bump(id);
  for (auto& [id, _] : s.bodies) bump(id);
  for (auto& [id, _] : s.ships) bump(id);
  for (auto& [id, _] : s.colonies) bump(id);
  for (auto& [id, _] : s.factions) bump(id);
  if (s.next_id <= max_id) s.next_id = max_id + 1;

  return s;
}

} // namespace nebula4x
