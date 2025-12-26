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

const json::Value* find_key(const json::Object& o, const std::string& k) {
  auto it = o.find(k);
  return it == o.end() ? nullptr : &it->second;
}

} // namespace

ContentDB load_content_db_from_file(const std::string& path) {
  const auto txt = read_text_file(path);
  const auto root = json::parse(txt).object();

  ContentDB db;

  const auto& comps = root.at("components").object();

  // Installations
  const auto& inst = root.at("installations").object();
  for (const auto& [inst_id, v] : inst) {
    const auto& vo = v.object();
    InstallationDef def;
    def.id = inst_id;
    if (const auto* name_v = find_key(vo, "name")) def.name = name_v->string_value(inst_id);
    else def.name = inst_id;

    if (const auto* prod_v = find_key(vo, "produces")) {
      for (const auto& [mineral, amount_v] : prod_v->object()) {
        def.produces_per_day[mineral] = amount_v.number_value(0.0);
      }
    }

    if (const auto* rate_v = find_key(vo, "build_rate_tons_per_day")) {
      def.build_rate_tons_per_day = rate_v->number_value(0.0);
    }

    db.installations[def.id] = def;
  }

  // Designs
  const auto& designs = root.at("designs").array();
  for (const auto& dj : designs) {
    const auto& o = dj.object();
    ShipDesign d;
    d.id = o.at("id").string_value();
    if (const auto* n = find_key(o, "name")) d.name = n->string_value(d.id);
    else d.name = d.id;
    if (const auto* r = find_key(o, "role")) d.role = parse_role(r->string_value("unknown"));

    // Components list
    for (const auto& cv : o.at("components").array()) d.components.push_back(cv.string_value());

    // Derive stats
    double mass = 0.0;
    double speed = 0.0;
    double cargo = 0.0;
    double sensor = 0.0;

    for (const auto& cid : d.components) {
      auto cit = comps.find(cid);
      if (cit == comps.end()) throw std::runtime_error("Unknown component id: " + cid);
      const auto& cj = cit->second.object();

      if (const auto* m = find_key(cj, "mass_tons")) mass += m->number_value(0.0);
      const std::string type = find_key(cj, "type") ? find_key(cj, "type")->string_value("") : "";

      if (type == "engine") {
        speed = std::max(speed, (find_key(cj, "speed_km_s") ? find_key(cj, "speed_km_s")->number_value(0.0) : 0.0));
      } else if (type == "cargo") {
        cargo += find_key(cj, "cargo_tons") ? find_key(cj, "cargo_tons")->number_value(0.0) : 0.0;
      } else if (type == "sensor") {
        sensor = std::max(sensor, find_key(cj, "range_mkm") ? find_key(cj, "range_mkm")->number_value(0.0) : 0.0);
      }
    }

    d.mass_tons = mass;
    d.speed_km_s = speed;
    d.cargo_tons = cargo;
    d.sensor_range_mkm = sensor;

    db.designs[d.id] = d;
  }

  return db;
}

} // namespace nebula4x
