#include "nebula4x/core/tech.h"

#include <algorithm>
#include <stdexcept>

#include "nebula4x/util/file_io.h"
#include "nebula4x/util/json.h"

namespace nebula4x {
namespace {

ShipRole parse_role(const std::string& s) {
  if (s == "freighter") return ShipRole::Freighter;
  if (s == "surveyor") return ShipRole::Surveyor;
  if (s == "combatant") return ShipRole::Combatant;
  return ShipRole::Unknown;
}

ComponentType parse_component_type(const std::string& s) {
  if (s == "engine") return ComponentType::Engine;
  if (s == "cargo") return ComponentType::Cargo;
  if (s == "sensor") return ComponentType::Sensor;
  if (s == "reactor") return ComponentType::Reactor;
  if (s == "weapon") return ComponentType::Weapon;
  if (s == "armor") return ComponentType::Armor;
  return ComponentType::Unknown;
}

const json::Value* find_key(const json::Object& o, const std::string& k) {
  auto it = o.find(k);
  return it == o.end() ? nullptr : &it->second;
}

} // namespace

ContentDB load_content_db_from_file(const std::string& path) {
  const auto txt = read_text_file(path);
  const auto root = json::parse(txt).object();

  ContentDB db;

  // --- Components ---
  if (auto itc = root.find("components"); itc != root.end()) {
    const auto& comps = itc->second.object();
    for (const auto& [cid, v] : comps) {
      const auto& cj = v.object();
      ComponentDef c;
      c.id = cid;
      c.name = find_key(cj, "name") ? find_key(cj, "name")->string_value(cid) : cid;
      c.type = parse_component_type(find_key(cj, "type") ? find_key(cj, "type")->string_value("") : "");
      if (const auto* m = find_key(cj, "mass_tons")) c.mass_tons = m->number_value(0.0);

      // Optional stats
      if (const auto* v_speed = find_key(cj, "speed_km_s")) c.speed_km_s = v_speed->number_value(0.0);
      if (const auto* v_cargo = find_key(cj, "cargo_tons")) c.cargo_tons = v_cargo->number_value(0.0);

      // Back-compat: sensors used to be "range_mkm".
      if (const auto* v_range = find_key(cj, "range_mkm")) c.sensor_range_mkm = v_range->number_value(0.0);
      if (const auto* v_sensor = find_key(cj, "sensor_range_mkm")) c.sensor_range_mkm = v_sensor->number_value(c.sensor_range_mkm);

      if (const auto* v_pow = find_key(cj, "power")) c.power = v_pow->number_value(0.0);

      if (const auto* v_dmg = find_key(cj, "damage")) c.weapon_damage = v_dmg->number_value(0.0);
      if (const auto* v_wr = find_key(cj, "weapon_range_mkm")) c.weapon_range_mkm = v_wr->number_value(0.0);
      if (const auto* v_wr2 = find_key(cj, "range_mkm")) {
        // If it's a weapon, allow using range_mkm for weapon range too.
        if (c.type == ComponentType::Weapon && c.weapon_range_mkm <= 0.0) {
          c.weapon_range_mkm = v_wr2->number_value(0.0);
        }
      }

      if (const auto* v_hp = find_key(cj, "hp_bonus")) c.hp_bonus = v_hp->number_value(0.0);

      db.components[c.id] = c;
    }
  }

  // --- Installations ---
  if (auto iti = root.find("installations"); iti != root.end()) {
    const auto& inst = iti->second.object();
    for (const auto& [inst_id, v] : inst) {
      const auto& vo = v.object();
      InstallationDef def;
      def.id = inst_id;
      def.name = find_key(vo, "name") ? find_key(vo, "name")->string_value(inst_id) : inst_id;

      if (const auto* prod_v = find_key(vo, "produces")) {
        for (const auto& [mineral, amount_v] : prod_v->object()) {
          def.produces_per_day[mineral] = amount_v.number_value(0.0);
        }
      }

      if (const auto* rate_v = find_key(vo, "build_rate_tons_per_day")) {
        def.build_rate_tons_per_day = rate_v->number_value(0.0);
      }

      // Optional: shipyard mineral input costs for shipbuilding.
      if (const auto* costs_v = find_key(vo, "build_costs_per_ton")) {
        for (const auto& [mineral, amount_v] : costs_v->object()) {
          def.build_costs_per_ton[mineral] = amount_v.number_value(0.0);
        }
      }

      if (const auto* rp_v = find_key(vo, "research_points_per_day")) {
        def.research_points_per_day = rp_v->number_value(0.0);
      }

      db.installations[def.id] = def;
    }
  }

  // --- Designs ---
  if (auto itd = root.find("designs"); itd != root.end()) {
    const auto& designs = itd->second.array();
    for (const auto& dj : designs) {
      const auto& o = dj.object();
      ShipDesign d;
      d.id = o.at("id").string_value();
      d.name = find_key(o, "name") ? find_key(o, "name")->string_value(d.id) : d.id;
      d.role = find_key(o, "role") ? parse_role(find_key(o, "role")->string_value("unknown")) : ShipRole::Unknown;

      for (const auto& cv : o.at("components").array()) d.components.push_back(cv.string_value());

      // Derive stats.
      double mass = 0.0;
      double speed = 0.0;
      double cargo = 0.0;
      double sensor = 0.0;
      double weapon_damage = 0.0;
      double weapon_range = 0.0;
      double hp_bonus = 0.0;

      for (const auto& cid : d.components) {
        auto cit = db.components.find(cid);
        if (cit == db.components.end()) throw std::runtime_error("Unknown component id: " + cid);
        const auto& c = cit->second;

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

      d.mass_tons = mass;
      d.speed_km_s = speed;
      d.cargo_tons = cargo;
      d.sensor_range_mkm = sensor;
      d.weapon_damage = weapon_damage;
      d.weapon_range_mkm = weapon_range;

      // Very rough survivability model for the prototype.
      // (Later you can split this into armor/structure/etc.)
      d.max_hp = std::max(1.0, mass * 2.0 + hp_bonus);

      db.designs[d.id] = d;
    }
  }

  return db;
}

std::unordered_map<std::string, TechDef> load_tech_db_from_file(const std::string& path) {
  const auto txt = read_text_file(path);
  const auto root = json::parse(txt).object();

  std::unordered_map<std::string, TechDef> out;

  auto it = root.find("techs");
  if (it == root.end()) return out;

  for (const auto& tv : it->second.array()) {
    const auto& o = tv.object();
    TechDef t;
    t.id = o.at("id").string_value();
    t.name = find_key(o, "name") ? find_key(o, "name")->string_value(t.id) : t.id;
    t.cost = find_key(o, "cost") ? find_key(o, "cost")->number_value(0.0) : 0.0;

    if (const auto* pv = find_key(o, "prereqs")) {
      for (const auto& p : pv->array()) t.prereqs.push_back(p.string_value());
    }

    if (const auto* ev = find_key(o, "effects")) {
      for (const auto& e : ev->array()) {
        const auto& eo = e.object();
        TechEffect eff;
        eff.type = find_key(eo, "type") ? find_key(eo, "type")->string_value("") : "";
        eff.value = find_key(eo, "value") ? find_key(eo, "value")->string_value("") : "";
        if (const auto* av = find_key(eo, "amount")) eff.amount = av->number_value(0.0);
        t.effects.push_back(eff);
      }
    }

    out[t.id] = t;
  }

  return out;
}

} // namespace nebula4x
