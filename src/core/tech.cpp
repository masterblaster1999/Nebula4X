#include "nebula4x/core/tech.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

#include "nebula4x/util/file_io.h"
#include "nebula4x/util/json.h"
#include "nebula4x/util/json_merge_patch.h"

namespace nebula4x {
namespace {
namespace fs = std::filesystem;

std::string ascii_to_lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return s;
}

ShipRole parse_role(const std::string& s) {
  if (s == "freighter") return ShipRole::Freighter;
  if (s == "surveyor") return ShipRole::Surveyor;
  if (s == "combatant") return ShipRole::Combatant;
  return ShipRole::Unknown;
}

ComponentType parse_component_type(const std::string& s) {
  if (s == "engine") return ComponentType::Engine;
  if (s == "fuel_tank" || s == "fuel") return ComponentType::FuelTank;
  if (s == "cargo") return ComponentType::Cargo;
  if (s == "mining") return ComponentType::Mining;
  if (s == "sensor") return ComponentType::Sensor;
  if (s == "reactor") return ComponentType::Reactor;
  if (s == "weapon") return ComponentType::Weapon;
  if (s == "armor") return ComponentType::Armor;
  if (s == "shield") return ComponentType::Shield;
  if (s == "troop_bay" || s == "troops") return ComponentType::TroopBay;
  if (s == "colony_module" || s == "colony") return ComponentType::ColonyModule;
  return ComponentType::Unknown;
}

const json::Value* find_key(const json::Object& o, const std::string& k) {
  auto it = o.find(k);
  return it == o.end() ? nullptr : &it->second;
}

bool bool_key(const json::Object& o, const std::string& k, bool def = false) {
  if (const auto* v = find_key(o, k)) return v->bool_value(def);
  return def;
}

std::vector<std::string> read_string_array(const json::Value& v, const std::string& what) {
  if (!v.is_array()) throw std::runtime_error(what + " must be an array");
  std::vector<std::string> out;
  out.reserve(v.array().size());
  for (const auto& x : v.array()) {
    if (!x.is_string()) throw std::runtime_error(what + " must contain only strings");
    out.push_back(x.string_value());
  }
  return out;
}

std::vector<std::string> read_includes(const json::Object& root, const std::string& file_path) {
  const json::Value* v = nullptr;
  if (auto it = root.find("include"); it != root.end()) v = &it->second;
  if (!v) {
    if (auto it = root.find("includes"); it != root.end()) v = &it->second;
  }
  if (!v) return {};

  std::vector<std::string> out;
  if (v->is_string()) {
    const std::string p = v->string_value();
    if (!p.empty()) out.push_back(p);
    return out;
  }
  if (v->is_array()) {
    for (const auto& el : v->array()) {
      if (!el.is_string()) {
        throw std::runtime_error("include/includes in " + file_path + " must contain only strings");
      }
      const std::string p = el.string_value();
      if (!p.empty()) out.push_back(p);
    }
    return out;
  }
  throw std::runtime_error("include/includes in " + file_path + " must be a string or array of strings");
}

std::string format_include_cycle(const std::vector<fs::path>& stack, const fs::path& repeat) {
  std::ostringstream oss;
  oss << "Include cycle detected:\n";
  for (const auto& p : stack) oss << "  -> " << p.string() << "\n";
  oss << "  -> " << repeat.string() << "\n";
  return oss.str();
}

// NOTE: Merge patch semantics are implemented as a shared utility (RFC 7386).
// Core data loaders use this to support layered content/tech overlays.

void apply_string_list_patch(json::Object& merged,
                            const json::Object& patch,
                            const std::string& list_key,
                            const std::string& add_key,
                            const std::string& remove_key,
                            const std::string& ctx) {
  const bool has_add = (patch.find(add_key) != patch.end());
  const bool has_remove = (patch.find(remove_key) != patch.end());
  if (!has_add && !has_remove) return;

  // Current list.
  std::vector<std::string> cur;
  if (auto it = merged.find(list_key); it != merged.end()) {
    if (!it->second.is_array()) throw std::runtime_error(ctx + ": " + list_key + " must be an array");
    cur = read_string_array(it->second, ctx + ": " + list_key);
  }

  std::unordered_set<std::string> remove_set;
  if (has_remove) {
    const auto& rv = patch.at(remove_key);
    for (const auto& s : read_string_array(rv, ctx + ": " + remove_key)) remove_set.insert(s);
  }

  std::unordered_set<std::string> seen;
  std::vector<std::string> out;
  out.reserve(cur.size() + 8);

  // Keep existing order.
  for (const auto& s : cur) {
    if (remove_set.find(s) != remove_set.end()) continue;
    if (seen.insert(s).second) out.push_back(s);
  }

  // Append additions in order.
  if (has_add) {
    const auto& av = patch.at(add_key);
    for (const auto& s : read_string_array(av, ctx + ": " + add_key)) {
      if (remove_set.find(s) != remove_set.end()) continue;
      if (seen.insert(s).second) out.push_back(s);
    }
  }

  json::Array arr;
  arr.reserve(out.size());
  for (const auto& s : out) arr.push_back(s);
  merged[list_key] = json::array(std::move(arr));
}

void strip_keys(json::Object& o, std::initializer_list<const char*> keys) {
  for (const char* k : keys) o.erase(k);
}

void collect_includes_dfs(const fs::path& path_in,
                          std::unordered_set<std::string>& visited,
                          std::vector<fs::path>& stack,
                          std::vector<fs::path>& out) {
  const fs::path path = fs::absolute(path_in).lexically_normal();
  const std::string key = path.generic_string();

  // Cycle detection (stack).
  for (const auto& s : stack) {
    if (s == path) {
      throw std::runtime_error(format_include_cycle(stack, path));
    }
  }

  // De-dupe.
  if (visited.find(key) != visited.end()) return;
  visited.insert(key);

  // Parse includes.
  const std::string txt = read_text_file(path.string());
  const auto rootv = json::parse(txt);
  if (!rootv.is_object()) {
    throw std::runtime_error("JSON root must be an object in " + path.string());
  }
  const auto& root = rootv.object();

  stack.push_back(path);

  const auto includes = read_includes(root, path.string());
  for (const auto& inc : includes) {
    fs::path child(inc);
    if (child.is_relative()) child = path.parent_path() / child;
    collect_includes_dfs(child, visited, stack, out);
  }

  out.push_back(path);

  stack.pop_back();
}

std::vector<fs::path> expand_roots_with_includes(const std::vector<std::string>& roots) {
  std::vector<fs::path> out;
  std::unordered_set<std::string> visited;
  std::vector<fs::path> stack;

  for (const auto& r : roots) {
    if (r.empty()) continue;
    collect_includes_dfs(fs::path(r), visited, stack, out);
  }
  return out;
}

struct SourcedValue {
  json::Value value;
  fs::path source;
};

// Merge blueprint documents (resources/components/installations/designs) into a single set of raw JSON values.
struct RawBlueprintAggregate {
  std::unordered_map<std::string, SourcedValue> resources;
  std::unordered_map<std::string, SourcedValue> components;
  std::unordered_map<std::string, SourcedValue> installations;
  std::unordered_map<std::string, SourcedValue> designs;
};

bool is_delete_marker(const json::Object& o) {
  return bool_key(o, "delete", false) || bool_key(o, "remove", false);
}

void merge_blueprint_file(RawBlueprintAggregate& agg, const fs::path& file) {
  const std::string txt = read_text_file(file.string());
  const auto rootv = json::parse(txt);
  if (!rootv.is_object()) throw std::runtime_error("Blueprint JSON root must be an object: " + file.string());
  const auto& root = rootv.object();

  // --- Resources (object of objects) ---
  if (auto itr = root.find("resources"); itr != root.end()) {
    if (!itr->second.is_object()) throw std::runtime_error("resources must be an object in " + file.string());
    const auto& res = itr->second.object();
    for (const auto& [rid, rv] : res) {
      if (rv.is_null()) {
        agg.resources.erase(rid);
        continue;
      }
      if (!rv.is_object()) throw std::runtime_error("resource '" + rid + "' must be an object in " + file.string());
      if (is_delete_marker(rv.object())) {
        agg.resources.erase(rid);
        continue;
      }

      auto it = agg.resources.find(rid);
      if (it == agg.resources.end()) {
        agg.resources[rid] = SourcedValue{rv, file};
        continue;
      }

      json::Value merged = it->second.value;
      apply_json_merge_patch(merged, rv);
      if (!merged.is_object()) {
        throw std::runtime_error("internal error: merged resource is not an object for id '" + rid + "'");
      }
      auto& mo = *merged.as_object();
      strip_keys(mo, {"delete", "remove"});
      agg.resources[rid] = SourcedValue{merged, file};
    }
  }

  // --- Components (object of objects) ---
  if (auto itc = root.find("components"); itc != root.end()) {
    if (!itc->second.is_object()) throw std::runtime_error("components must be an object in " + file.string());
    const auto& comps = itc->second.object();
    for (const auto& [cid, cv] : comps) {
      if (cv.is_null()) {
        agg.components.erase(cid);
        continue;
      }
      if (!cv.is_object()) throw std::runtime_error("component '" + cid + "' must be an object in " + file.string());
      if (is_delete_marker(cv.object())) {
        agg.components.erase(cid);
        continue;
      }

      auto it = agg.components.find(cid);
      if (it == agg.components.end()) {
        agg.components[cid] = SourcedValue{cv, file};
      } else {
        json::Value merged = it->second.value;
        apply_json_merge_patch(merged, cv);
        agg.components[cid] = SourcedValue{merged, file};
      }
    }
  }

  // --- Installations (object of objects) ---
  if (auto iti = root.find("installations"); iti != root.end()) {
    if (!iti->second.is_object()) throw std::runtime_error("installations must be an object in " + file.string());
    const auto& inst = iti->second.object();
    for (const auto& [iid, iv] : inst) {
      if (iv.is_null()) {
        agg.installations.erase(iid);
        continue;
      }
      if (!iv.is_object()) throw std::runtime_error("installation '" + iid + "' must be an object in " + file.string());
      if (is_delete_marker(iv.object())) {
        agg.installations.erase(iid);
        continue;
      }

      auto it = agg.installations.find(iid);
      if (it == agg.installations.end()) {
        agg.installations[iid] = SourcedValue{iv, file};
      } else {
        json::Value merged = it->second.value;
        apply_json_merge_patch(merged, iv);
        agg.installations[iid] = SourcedValue{merged, file};
      }
    }
  }

  // --- Designs (array of objects) ---
  if (auto itd = root.find("designs"); itd != root.end()) {
    if (!itd->second.is_array()) throw std::runtime_error("designs must be an array in " + file.string());
    for (const auto& dv : itd->second.array()) {
      if (!dv.is_object()) throw std::runtime_error("design entry must be an object in " + file.string());
      const auto& o = dv.object();
      const std::string id = find_key(o, "id") ? find_key(o, "id")->string_value("") : "";
      if (id.empty()) throw std::runtime_error("design entry missing id in " + file.string());

      if (is_delete_marker(o)) {
        agg.designs.erase(id);
        continue;
      }

      auto it = agg.designs.find(id);
      json::Value merged = (it == agg.designs.end()) ? dv : it->second.value;
      apply_json_merge_patch(merged, dv);

      if (!merged.is_object()) {
        throw std::runtime_error("internal error: merged design is not an object for id '" + id + "'");
      }
      auto& mo = *merged.as_object();

      apply_string_list_patch(mo, o, "components", "components_add", "components_remove",
                              "design '" + id + "' (" + file.string() + ")");

      strip_keys(mo, {"components_add", "components_remove", "delete", "remove"});
      agg.designs[id] = SourcedValue{merged, file};
    }
  }
}

// Merge tech tree documents (techs array of objects) into a single set of raw JSON values.
struct RawTechAggregate {
  std::unordered_map<std::string, SourcedValue> techs;
};

void merge_tech_file(RawTechAggregate& agg, const fs::path& file) {
  const std::string txt = read_text_file(file.string());
  const auto rootv = json::parse(txt);
  if (!rootv.is_object()) throw std::runtime_error("Tech JSON root must be an object: " + file.string());
  const auto& root = rootv.object();

  auto it = root.find("techs");
  if (it == root.end()) return;
  if (!it->second.is_array()) throw std::runtime_error("techs must be an array in " + file.string());

  for (const auto& tv : it->second.array()) {
    if (!tv.is_object()) throw std::runtime_error("tech entry must be an object in " + file.string());
    const auto& o = tv.object();
    const std::string id = find_key(o, "id") ? find_key(o, "id")->string_value("") : "";
    if (id.empty()) throw std::runtime_error("tech entry missing id in " + file.string());

    if (is_delete_marker(o)) {
      agg.techs.erase(id);
      continue;
    }

    auto itx = agg.techs.find(id);
    json::Value merged = (itx == agg.techs.end()) ? tv : itx->second.value;
    apply_json_merge_patch(merged, tv);

    if (!merged.is_object()) {
      throw std::runtime_error("internal error: merged tech is not an object for id '" + id + "'");
    }
    auto& mo = *merged.as_object();

    apply_string_list_patch(mo, o, "prereqs", "prereqs_add", "prereqs_remove",
                            "tech '" + id + "' (" + file.string() + ")");

    strip_keys(mo, {"prereqs_add", "prereqs_remove", "delete", "remove"});
    agg.techs[id] = SourcedValue{merged, file};
  }
}

ResourceDef parse_resource_def(const std::string& rid, const json::Object& rj) {
  ResourceDef r;
  r.id = rid;
  r.name = find_key(rj, "name") ? find_key(rj, "name")->string_value(rid) : rid;
  if (const auto* cv = find_key(rj, "category")) r.category = cv->string_value("mineral");
  if (r.category == "mineral") {
    // Alias: some mods may prefer "type".
    if (const auto* tv = find_key(rj, "type")) r.category = tv->string_value("mineral");
  }
  if (r.category.empty()) r.category = "mineral";
  r.mineable = bool_key(rj, "mineable", true);
  return r;
}

ComponentDef parse_component_def(const std::string& cid, const json::Object& cj) {
  ComponentDef c;
  c.id = cid;
  c.name = find_key(cj, "name") ? find_key(cj, "name")->string_value(cid) : cid;
  c.type = parse_component_type(find_key(cj, "type") ? find_key(cj, "type")->string_value("") : "");
  if (const auto* m = find_key(cj, "mass_tons")) c.mass_tons = m->number_value(0.0);

  // Optional: sensor signature / visibility multiplier.
  // 1.0 = normal visibility; lower values are harder to detect.
  if (const auto* v_sig = find_key(cj, "signature_multiplier")) {
    c.signature_multiplier = v_sig->number_value(1.0);
  }
  if (const auto* v_sig2 = find_key(cj, "stealth_multiplier")) {
    if (c.signature_multiplier == 1.0) c.signature_multiplier = v_sig2->number_value(1.0);
  }
  if (const auto* v_sig3 = find_key(cj, "signature")) {
    if (c.signature_multiplier == 1.0) c.signature_multiplier = v_sig3->number_value(1.0);
  }

  // Optional stats
  if (const auto* v_speed = find_key(cj, "speed_km_s")) c.speed_km_s = v_speed->number_value(0.0);

  // Optional: fuel mechanics
  // - fuel_use_per_mkm is interpreted as tons of fuel consumed per million km traveled.
  // - fuel_capacity_tons is the shipboard tank capacity in fuel tons.
  if (const auto* v_fuse = find_key(cj, "fuel_use_per_mkm")) c.fuel_use_per_mkm = v_fuse->number_value(0.0);
  if (const auto* v_fuse2 = find_key(cj, "fuel_use")) {
    if (c.fuel_use_per_mkm <= 0.0) c.fuel_use_per_mkm = v_fuse2->number_value(0.0);
  }
  if (const auto* v_fcap = find_key(cj, "fuel_capacity_tons")) c.fuel_capacity_tons = v_fcap->number_value(0.0);
  if (const auto* v_fcap2 = find_key(cj, "fuel_tons")) {
    if (c.fuel_capacity_tons <= 0.0) c.fuel_capacity_tons = v_fcap2->number_value(0.0);
  }
  if (const auto* v_fcap3 = find_key(cj, "fuel_capacity")) {
    if (c.fuel_capacity_tons <= 0.0) c.fuel_capacity_tons = v_fcap3->number_value(0.0);
  }

  if (const auto* v_cargo = find_key(cj, "cargo_tons")) c.cargo_tons = v_cargo->number_value(0.0);

  // Optional: mobile mining.
  if (const auto* v_mine = find_key(cj, "mining_tons_per_day")) c.mining_tons_per_day = v_mine->number_value(0.0);
  if (const auto* v_mine2 = find_key(cj, "mining_rate_tpd")) {
    if (c.mining_tons_per_day <= 0.0) c.mining_tons_per_day = v_mine2->number_value(0.0);
  }

  // Back-compat: sensors used to be "range_mkm".
  if (const auto* v_range = find_key(cj, "range_mkm")) c.sensor_range_mkm = v_range->number_value(0.0);
  if (const auto* v_sensor = find_key(cj, "sensor_range_mkm")) c.sensor_range_mkm = v_sensor->number_value(c.sensor_range_mkm);

  // Optional: colony module seeding capacity.
  if (const auto* v_col = find_key(cj, "colony_capacity_millions")) {
    c.colony_capacity_millions = v_col->number_value(0.0);
  }

  // Optional: troop bay capacity.
  if (const auto* v_tc = find_key(cj, "troop_capacity")) c.troop_capacity = v_tc->number_value(0.0);

  // Power model (prototype):
  // - Reactors contribute positive power_output.
  // - Other components may draw power_use.
  // Back-compat: allow using "power" as output for reactors, or as
  // consumption for non-reactor components.
  if (const auto* v_out = find_key(cj, "power_output")) c.power_output = v_out->number_value(0.0);
  if (const auto* v_use = find_key(cj, "power_use")) c.power_use = v_use->number_value(0.0);
  if (const auto* v_use2 = find_key(cj, "power_draw")) {
    if (c.power_use <= 0.0) c.power_use = v_use2->number_value(0.0);
  }
  if (const auto* v_use3 = find_key(cj, "power_consumption")) {
    if (c.power_use <= 0.0) c.power_use = v_use3->number_value(0.0);
  }
  if (const auto* v_pow = find_key(cj, "power")) {
    if (c.type == ComponentType::Reactor) {
      if (c.power_output <= 0.0) c.power_output = v_pow->number_value(0.0);
    } else {
      if (c.power_use <= 0.0) c.power_use = v_pow->number_value(0.0);
    }
  }

  if (const auto* v_dmg = find_key(cj, "damage")) c.weapon_damage = v_dmg->number_value(0.0);
  if (const auto* v_wr = find_key(cj, "weapon_range_mkm")) c.weapon_range_mkm = v_wr->number_value(0.0);
  if (const auto* v_wr2 = find_key(cj, "range_mkm")) {
    // If it's a weapon, allow using range_mkm for weapon range too.
    if (c.type == ComponentType::Weapon && c.weapon_range_mkm <= 0.0) {
      c.weapon_range_mkm = v_wr2->number_value(0.0);
    }
  }

  if (const auto* v_hp = find_key(cj, "hp_bonus")) c.hp_bonus = v_hp->number_value(0.0);

  // Optional: shields.
  if (const auto* v_sh = find_key(cj, "shield_hp")) c.shield_hp = v_sh->number_value(0.0);
  if (const auto* v_sh2 = find_key(cj, "shield")) {
    // Allow shorthand "shield" for capacity in some content.
    if (c.shield_hp <= 0.0) c.shield_hp = v_sh2->number_value(0.0);
  }
  if (const auto* v_sr = find_key(cj, "shield_regen_per_day")) c.shield_regen_per_day = v_sr->number_value(0.0);
  if (const auto* v_sr2 = find_key(cj, "shield_regen")) {
    // Allow shorthand "shield_regen".
    if (c.shield_regen_per_day <= 0.0) c.shield_regen_per_day = v_sr2->number_value(0.0);
  }

  return c;
}

InstallationDef parse_installation_def(const std::string& inst_id, const json::Object& vo) {
  InstallationDef def;
  def.id = inst_id;
  def.name = find_key(vo, "name") ? find_key(vo, "name")->string_value(inst_id) : inst_id;

  if (const auto* prod_v = find_key(vo, "produces")) {
    if (!prod_v->is_object()) {
      throw std::runtime_error("Installation '" + inst_id + "': produces must be an object");
    }
    for (const auto& [mineral, amount_v] : prod_v->object()) {
      def.produces_per_day[mineral] = amount_v.number_value(0.0);
    }
  }

  // Optional "industry" inputs: minerals consumed per day.
  // Supported keys: consumes (preferred), consumes_per_day (alias/back-compat).
  auto parse_consumes_obj = [&](const json::Value* v) {
    if (!v) return;
    if (!v->is_object()) {
      throw std::runtime_error("Installation '" + inst_id + "': consumes must be an object");
    }
    for (const auto& [mineral, amount_v] : v->object()) {
      def.consumes_per_day[mineral] = amount_v.number_value(0.0);
    }
  };

  parse_consumes_obj(find_key(vo, "consumes"));
  parse_consumes_obj(find_key(vo, "consumes_per_day"));

  // Optional: mark this installation as a mining extractor.
  // If not explicitly specified, fall back to a simple heuristic for back-compat
  // content: any installation whose id contains "mine" and that produces minerals
  // is treated as a miner.
  bool mining_explicit = false;
  if (const auto* mv = find_key(vo, "mining")) {
    def.mining = mv->bool_value(false);
    mining_explicit = true;
  } else if (const auto* mv2 = find_key(vo, "extracts_deposits")) {
    def.mining = mv2->bool_value(false);
    mining_explicit = true;
  }

  if (!mining_explicit && !def.produces_per_day.empty()) {
    const std::string lid = ascii_to_lower(def.id);
    if (lid.find("mine") != std::string::npos) {
      def.mining = true;
    }
  }

  // Optional: generic mining capacity (tons per day).
  // When set > 0, mining installations distribute this capacity across all
  // non-depleted deposits on the colony's body (weighted by remaining tons).
  if (const auto* mt_v = find_key(vo, "mining_tons_per_day")) def.mining_tons_per_day = mt_v->number_value(0.0);
  if (const auto* mt_v2 = find_key(vo, "mining_rate_tpd")) {
    if (def.mining_tons_per_day <= 0.0) def.mining_tons_per_day = mt_v2->number_value(0.0);
  }
  if (const auto* mt_v3 = find_key(vo, "mining_capacity_tpd")) {
    if (def.mining_tons_per_day <= 0.0) def.mining_tons_per_day = mt_v3->number_value(0.0);
  }
  if (const auto* mt_v4 = find_key(vo, "mining_tpd")) {
    if (def.mining_tons_per_day <= 0.0) def.mining_tons_per_day = mt_v4->number_value(0.0);
  }
  if (def.mining_tons_per_day > 0.0) def.mining = true;


  if (const auto* cp_v = find_key(vo, "construction_points_per_day")) {
    def.construction_points_per_day = cp_v->number_value(0.0);
  }

  if (const auto* cc_v = find_key(vo, "construction_cost")) {
    def.construction_cost = cc_v->number_value(0.0);
  }

  if (const auto* bc_v = find_key(vo, "build_costs")) {
    for (const auto& [mineral, amount_v] : bc_v->object()) {
      def.build_costs[mineral] = amount_v.number_value(0.0);
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

  // Optional: in-system sensor range for sensor installations.
  if (const auto* sr_v = find_key(vo, "sensor_range_mkm")) {
    def.sensor_range_mkm = sr_v->number_value(0.0);
  }
  if (const auto* sr2_v = find_key(vo, "range_mkm")) {
    if (def.sensor_range_mkm <= 0.0) def.sensor_range_mkm = sr2_v->number_value(0.0);
  }

  // Optional: orbital / planetary weapon platform.
  // Preferred keys:
  //   weapon_damage, weapon_range_mkm
  // Back-compat / author-friendly aliases:
  //   damage (alias for weapon_damage)
  //   weapon_range (alias for weapon_range_mkm)
  if (const auto* wd_v = find_key(vo, "weapon_damage")) {
    def.weapon_damage = wd_v->number_value(0.0);
  }
  if (const auto* dmg_v = find_key(vo, "damage")) {
    if (def.weapon_damage <= 0.0) def.weapon_damage = dmg_v->number_value(0.0);
  }

  if (const auto* wr_v = find_key(vo, "weapon_range_mkm")) {
    def.weapon_range_mkm = wr_v->number_value(0.0);
  }
  if (const auto* wr2_v = find_key(vo, "weapon_range")) {
    if (def.weapon_range_mkm <= 0.0) def.weapon_range_mkm = wr2_v->number_value(0.0);
  }

  if (const auto* rp_v = find_key(vo, "research_points_per_day")) {
    def.research_points_per_day = rp_v->number_value(0.0);
  }

  if (const auto* tf_v = find_key(vo, "terraforming_points_per_day")) {
    def.terraforming_points_per_day = tf_v->number_value(0.0);
  }
  if (const auto* tr_v = find_key(vo, "troop_training_points_per_day")) {
    def.troop_training_points_per_day = tr_v->number_value(0.0);
  }

  // Optional: habitation / life support capacity.
  // Preferred key: habitation_capacity_millions
  // Aliases: habitation_capacity, life_support_capacity_millions
  if (const auto* hc_v = find_key(vo, "habitation_capacity_millions")) {
    def.habitation_capacity_millions = hc_v->number_value(0.0);
  }
  if (const auto* hc2_v = find_key(vo, "habitation_capacity")) {
    if (def.habitation_capacity_millions <= 0.0) def.habitation_capacity_millions = hc2_v->number_value(0.0);
  }
  if (const auto* ls_v = find_key(vo, "life_support_capacity_millions")) {
    if (def.habitation_capacity_millions <= 0.0) def.habitation_capacity_millions = ls_v->number_value(0.0);
  }
  if (const auto* fort_v = find_key(vo, "fortification_points")) {
    def.fortification_points = fort_v->number_value(0.0);
  }

  return def;
}

ShipDesign parse_design_def(const json::Object& o,
                            const ContentDB& db,
                            const std::string& file_hint) {
  ShipDesign d;
  d.id = o.at("id").string_value();
  d.name = find_key(o, "name") ? find_key(o, "name")->string_value(d.id) : d.id;
  d.role = find_key(o, "role") ? parse_role(find_key(o, "role")->string_value("unknown")) : ShipRole::Unknown;

  const auto* comps_v = find_key(o, "components");
  if (!comps_v || !comps_v->is_array()) {
    throw std::runtime_error("Design '" + d.id + "' missing components array (" + file_hint + ")");
  }
  for (const auto& cv : comps_v->array()) d.components.push_back(cv.string_value());

  // Derive stats.
  double mass = 0.0;
  double speed = 0.0;
  double fuel_cap = 0.0;
  double fuel_use = 0.0;
  double cargo = 0.0;
  double mining_rate = 0.0;
  double sensor = 0.0;
  double colony_cap = 0.0;
  double troop_cap = 0.0;

  // Visibility / signature multiplier (product of component multipliers).
  // 1.0 = normal visibility; lower values are harder to detect.
  double sig_mult = 1.0;

  double weapon_damage = 0.0;
  double weapon_range = 0.0;
  double hp_bonus = 0.0;
  double max_shields = 0.0;
  double shield_regen = 0.0;

  // Power budgeting.
  double power_gen = 0.0;
  double power_use_total = 0.0;
  double power_use_engines = 0.0;
  double power_use_sensors = 0.0;
  double power_use_weapons = 0.0;
  double power_use_shields = 0.0;

  for (const auto& cid : d.components) {
    auto cit = db.components.find(cid);
    if (cit == db.components.end()) {
      throw std::runtime_error("Unknown component id '" + cid + "' in design '" + d.id + "' (" + file_hint + ")");
    }
    const auto& c = cit->second;

    mass += c.mass_tons;
    speed = std::max(speed, c.speed_km_s);
    fuel_cap += c.fuel_capacity_tons;
    fuel_use += c.fuel_use_per_mkm;
    cargo += c.cargo_tons;
    mining_rate += c.mining_tons_per_day;
    sensor = std::max(sensor, c.sensor_range_mkm);
    colony_cap += c.colony_capacity_millions;
    troop_cap += c.troop_capacity;

    const double comp_sig =
        std::clamp(std::isfinite(c.signature_multiplier) ? c.signature_multiplier : 1.0, 0.0, 1.0);
    sig_mult *= comp_sig;

    if (c.type == ComponentType::Weapon) {
      weapon_damage += c.weapon_damage;
      weapon_range = std::max(weapon_range, c.weapon_range_mkm);
    }

    if (c.type == ComponentType::Reactor) {
      power_gen += c.power_output;
    }
    power_use_total += c.power_use;
    if (c.type == ComponentType::Engine) power_use_engines += c.power_use;
    if (c.type == ComponentType::Sensor) power_use_sensors += c.power_use;
    if (c.type == ComponentType::Weapon) power_use_weapons += c.power_use;
    if (c.type == ComponentType::Shield) power_use_shields += c.power_use;

    hp_bonus += c.hp_bonus;

    if (c.type == ComponentType::Shield) {
      max_shields += c.shield_hp;
      shield_regen += c.shield_regen_per_day;
    }
  }

  d.mass_tons = mass;
  d.speed_km_s = speed;
  d.fuel_capacity_tons = fuel_cap;
  d.fuel_use_per_mkm = fuel_use;
  d.cargo_tons = cargo;
  d.mining_tons_per_day = mining_rate;
  d.sensor_range_mkm = sensor;
  d.colony_capacity_millions = colony_cap;
  d.troop_capacity = troop_cap;
  // Clamp to avoid fully-undetectable ships.
  d.signature_multiplier = std::clamp(sig_mult, 0.05, 1.0);

  d.power_generation = power_gen;
  d.power_use_total = power_use_total;
  d.power_use_engines = power_use_engines;
  d.power_use_sensors = power_use_sensors;
  d.power_use_weapons = power_use_weapons;
  d.power_use_shields = power_use_shields;
  d.weapon_damage = weapon_damage;
  d.weapon_range_mkm = weapon_range;

  d.max_shields = max_shields;
  d.shield_regen_per_day = shield_regen;

  // Very rough survivability model for the prototype.
  // (Later you can split this into armor/structure/etc.)
  d.max_hp = std::max(1.0, mass * 2.0 + hp_bonus);

  return d;
}

TechDef parse_tech_def(const json::Object& o, const std::string& file_hint) {
  TechDef t;
  t.id = o.at("id").string_value();
  t.name = find_key(o, "name") ? find_key(o, "name")->string_value(t.id) : t.id;
  t.cost = find_key(o, "cost") ? find_key(o, "cost")->number_value(0.0) : 0.0;

  if (const auto* pv = find_key(o, "prereqs")) {
    if (!pv->is_array()) throw std::runtime_error("tech '" + t.id + "': prereqs must be an array (" + file_hint + ")");
    for (const auto& p : pv->array()) t.prereqs.push_back(p.string_value());
  }

  if (const auto* ev = find_key(o, "effects")) {
    if (!ev->is_array()) throw std::runtime_error("tech '" + t.id + "': effects must be an array (" + file_hint + ")");
    for (const auto& e : ev->array()) {
      if (!e.is_object()) throw std::runtime_error("tech '" + t.id + "': effect must be an object (" + file_hint + ")");
      const auto& eo = e.object();
      TechEffect eff;
      eff.type = find_key(eo, "type") ? find_key(eo, "type")->string_value("") : "";
      eff.value = find_key(eo, "value") ? find_key(eo, "value")->string_value("") : "";
      if (const auto* av = find_key(eo, "amount")) eff.amount = av->number_value(0.0);
      t.effects.push_back(eff);
    }
  }

  return t;
}

} // namespace

ContentDB load_content_db_from_file(const std::string& path) {
  return load_content_db_from_files({path});
}

ContentDB load_content_db_from_files(const std::vector<std::string>& paths) {
  ContentDB db;
  if (paths.empty()) return db;

  const auto files = expand_roots_with_includes(paths);

  RawBlueprintAggregate agg;
  for (const auto& f : files) merge_blueprint_file(agg, f);

  // Resources (optional catalog).
  for (const auto& [rid, sv] : agg.resources) {
    if (!sv.value.is_object()) {
      throw std::runtime_error("resource '" + rid + "' is not an object (" + sv.source.string() + ")");
    }
    db.resources[rid] = parse_resource_def(rid, sv.value.object());
  }

  // Components first (designs depend on them).
  for (const auto& [cid, sv] : agg.components) {
    if (!sv.value.is_object()) {
      throw std::runtime_error("component '" + cid + "' is not an object (" + sv.source.string() + ")");
    }
    db.components[cid] = parse_component_def(cid, sv.value.object());
  }

  // Installations.
  for (const auto& [iid, sv] : agg.installations) {
    if (!sv.value.is_object()) {
      throw std::runtime_error("installation '" + iid + "' is not an object (" + sv.source.string() + ")");
    }
    db.installations[iid] = parse_installation_def(iid, sv.value.object());
  }

  // Designs.
  for (const auto& [did, sv] : agg.designs) {
    if (!sv.value.is_object()) {
      throw std::runtime_error("design '" + did + "' is not an object (" + sv.source.string() + ")");
    }
    db.designs[did] = parse_design_def(sv.value.object(), db, sv.source.string());
  }

  return db;
}

std::unordered_map<std::string, TechDef> load_tech_db_from_file(const std::string& path) {
  return load_tech_db_from_files({path});
}

std::unordered_map<std::string, TechDef> load_tech_db_from_files(const std::vector<std::string>& paths) {
  std::unordered_map<std::string, TechDef> out;
  if (paths.empty()) return out;

  const auto files = expand_roots_with_includes(paths);

  RawTechAggregate agg;
  for (const auto& f : files) merge_tech_file(agg, f);

  for (const auto& [tid, sv] : agg.techs) {
    if (!sv.value.is_object()) {
      throw std::runtime_error("tech '" + tid + "' is not an object (" + sv.source.string() + ")");
    }
    out[tid] = parse_tech_def(sv.value.object(), sv.source.string());
  }

  return out;
}

} // namespace nebula4x
