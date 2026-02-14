#include "ui/order_template_portable.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "nebula4x/core/game_state.h"
#include "nebula4x/core/simulation.h"
#include "nebula4x/util/json.h"
#include "nebula4x/util/strings.h"

namespace nebula4x::ui {
namespace {

using nebula4x::json::Array;
using nebula4x::json::Object;
using nebula4x::json::Value;

std::string to_lower_ascii(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool eq_ci(const std::string& a, const std::string& b) { return to_lower_ascii(a) == to_lower_ascii(b); }

struct RefResolver {
  const Simulation& sim;
  Id viewer_faction_id{kInvalidId};
  bool fog_of_war{false};

  bool allow_system(Id system_id) const {
    if (!fog_of_war || viewer_faction_id == kInvalidId) return true;
    return sim.is_system_discovered_by_faction(viewer_faction_id, system_id);
  }

  bool allow_jump_point(Id jump_point_id) const {
    if (!fog_of_war || viewer_faction_id == kInvalidId) return true;
    return sim.is_jump_point_surveyed_by_faction(viewer_faction_id, jump_point_id);
  }

  bool allow_ship(Id ship_id) const {
    if (!fog_of_war || viewer_faction_id == kInvalidId) return true;
    return sim.is_ship_detected_by_faction(viewer_faction_id, ship_id);
  }

  bool allow_anomaly(Id anomaly_id) const {
    if (!fog_of_war || viewer_faction_id == kInvalidId) return true;
    return sim.is_anomaly_discovered_by_faction(viewer_faction_id, anomaly_id);
  }
};

std::string system_name_if_visible(const RefResolver& rr, Id system_id) {
  const auto& st = rr.sim.state();
  const auto* sys = find_ptr(st.systems, system_id);
  if (!sys) return {};
  if (!rr.allow_system(system_id)) return {};
  return sys->name;
}

Object make_system_ref(const RefResolver& rr, Id system_id) {
  Object ref;
  const auto& st = rr.sim.state();
  const auto* sys = find_ptr(st.systems, system_id);
  if (!sys) return ref;
  if (!rr.allow_system(system_id)) return ref;

  ref["kind"] = std::string("system");
  if (!sys->name.empty()) ref["name"] = sys->name;
  return ref;
}

Object make_body_ref(const RefResolver& rr, Id body_id) {
  Object ref;
  const auto& st = rr.sim.state();
  const auto* b = find_ptr(st.bodies, body_id);
  if (!b) return ref;
  if (!rr.allow_system(b->system_id)) return ref;

  ref["kind"] = std::string("body");
  if (!b->name.empty()) ref["name"] = b->name;
  const std::string sys_name = system_name_if_visible(rr, b->system_id);
  if (!sys_name.empty()) ref["system"] = sys_name;
  return ref;
}

Object make_colony_ref(const RefResolver& rr, Id colony_id) {
  Object ref;
  const auto& st = rr.sim.state();
  const auto* c = find_ptr(st.colonies, colony_id);
  if (!c) return ref;

  const auto* b = find_ptr(st.bodies, c->body_id);
  if (b && !rr.allow_system(b->system_id)) return ref;

  ref["kind"] = std::string("colony");
  if (!c->name.empty()) ref["name"] = c->name;

  if (b && !b->name.empty()) {
    ref["body"] = b->name;
    const std::string sys_name = system_name_if_visible(rr, b->system_id);
    if (!sys_name.empty()) ref["system"] = sys_name;
  }

  if (const auto* f = find_ptr(st.factions, c->faction_id)) {
    if (!f->name.empty()) ref["faction"] = f->name;
  }

  return ref;
}

Object make_jump_point_ref(const RefResolver& rr, Id jump_point_id) {
  Object ref;
  const auto& st = rr.sim.state();
  const auto* jp = find_ptr(st.jump_points, jump_point_id);
  if (!jp) return ref;
  if (!rr.allow_system(jp->system_id)) return ref;

  ref["kind"] = std::string("jump_point");
  if (!jp->name.empty()) ref["name"] = jp->name;
  const std::string sys_name = system_name_if_visible(rr, jp->system_id);
  if (!sys_name.empty()) ref["system"] = sys_name;

  // Optional disambiguator: destination system name (only if visible).
  if (jp->linked_jump_id != kInvalidId) {
    if (const auto* other = find_ptr(st.jump_points, jp->linked_jump_id)) {
      const std::string dest = system_name_if_visible(rr, other->system_id);
      if (!dest.empty()) ref["dest_system"] = dest;
    }
  }

  return ref;
}

Object make_ship_ref(const RefResolver& rr, Id ship_id) {
  Object ref;
  const auto& st = rr.sim.state();
  const auto* sh = find_ptr(st.ships, ship_id);
  if (!sh) return ref;
  if (!rr.allow_ship(ship_id)) return ref;
  if (!rr.allow_system(sh->system_id)) return ref;

  ref["kind"] = std::string("ship");
  if (!sh->name.empty()) ref["name"] = sh->name;
  const std::string sys_name = system_name_if_visible(rr, sh->system_id);
  if (!sys_name.empty()) ref["system"] = sys_name;

  if (const auto* f = find_ptr(st.factions, sh->faction_id)) {
    if (!f->name.empty()) ref["faction"] = f->name;
  }

  return ref;
}

Object make_anomaly_ref(const RefResolver& rr, Id anomaly_id) {
  Object ref;
  const auto& st = rr.sim.state();
  const auto* a = find_ptr(st.anomalies, anomaly_id);
  if (!a) return ref;
  if (!rr.allow_anomaly(anomaly_id)) return ref;
  if (!rr.allow_system(a->system_id)) return ref;

  ref["kind"] = std::string("anomaly");
  if (!a->name.empty()) ref["name"] = a->name;
  const std::string sys_name = system_name_if_visible(rr, a->system_id);
  if (!sys_name.empty()) ref["system"] = sys_name;

  return ref;
}

Object make_wreck_ref(const RefResolver& rr, Id wreck_id) {
  Object ref;
  const auto& st = rr.sim.state();
  const auto* w = find_ptr(st.wrecks, wreck_id);
  if (!w) return ref;
  if (!rr.allow_system(w->system_id)) return ref;

  ref["kind"] = std::string("wreck");
  if (!w->name.empty()) ref["name"] = w->name;
  const std::string sys_name = system_name_if_visible(rr, w->system_id);
  if (!sys_name.empty()) ref["system"] = sys_name;

  return ref;
}

bool ref_has_name(const Object& ref) {
  if (auto it = ref.find("name"); it != ref.end()) {
    const std::string s = it->second.string_value();
    return !s.empty();
  }
  return false;
}

void add_portable_ref(Object& order_obj, const char* id_key, const char* ref_key, const Object& ref,
                      bool include_source_ids) {
  if (!ref_has_name(ref)) return;

  auto it = order_obj.find(id_key);
  if (it == order_obj.end()) return;

  const Value id_v = it->second;

  if (include_source_ids) {
    order_obj[std::string("source_") + id_key] = id_v;
  }

  // For v2 portability, remove the raw id and replace with a ref.
  order_obj.erase(it);
  order_obj[ref_key] = ref;
}

std::optional<Id> resolve_system_by_name(const RefResolver& rr, const std::string& system_name) {
  if (system_name.empty()) return std::nullopt;

  const auto& st = rr.sim.state();
  Id found = kInvalidId;
  int count = 0;
  for (const auto& [sid, sys] : st.systems) {
    if (!rr.allow_system(sys.id)) continue;
    if (eq_ci(sys.name, system_name)) {
      found = sys.id;
      ++count;
      if (count > 1) break;
    }
  }
  if (count == 1) return found;
  return std::nullopt;
}

template <typename MapT, typename Pred>
std::vector<Id> collect_ids_if(const MapT& m, Pred pred) {
  std::vector<Id> out;
  out.reserve(m.size());
  for (const auto& [id, v] : m) {
    if (pred(v)) out.push_back(id);
  }
  return out;
}

std::optional<Id> resolve_body_by_ref(const RefResolver& rr, const Object& ref, std::string* error) {
  const std::string body_name = ref.count("name") ? ref.at("name").string_value() : std::string();
  if (body_name.empty()) {
    if (error) *error = "body_ref missing name";
    return std::nullopt;
  }
  const std::string sys_name = ref.count("system") ? ref.at("system").string_value() : std::string();

  const auto& st = rr.sim.state();

  std::optional<Id> sys_id;
  if (!sys_name.empty()) {
    sys_id = resolve_system_by_name(rr, sys_name);
    if (!sys_id) {
      if (error) *error = "Unknown or undiscovered system '" + sys_name + "'";
      return std::nullopt;
    }
  }

  std::vector<Id> matches;
  for (const auto& [bid, b] : st.bodies) {
    if (!rr.allow_system(b.system_id)) continue;
    if (sys_id && b.system_id != *sys_id) continue;
    if (eq_ci(b.name, body_name)) matches.push_back(b.id);
  }

  if (matches.empty()) {
    if (error) {
      if (sys_id) {
        *error = "Body '" + body_name + "' not found in system '" + sys_name + "'";
      } else {
        *error = "Body '" + body_name + "' not found";
      }
    }
    return std::nullopt;
  }
  if (matches.size() > 1) {
    if (error) {
      *error = "Ambiguous body '" + body_name + "' (matches " + std::to_string(matches.size()) +
               "). Add 'system' to body_ref.";
    }
    return std::nullopt;
  }
  return matches.front();
}

std::optional<Id> resolve_colony_by_ref(const RefResolver& rr, const Object& ref, std::string* error) {
  const std::string colony_name = ref.count("name") ? ref.at("name").string_value() : std::string();
  const std::string body_name = ref.count("body") ? ref.at("body").string_value() : std::string();
  const std::string sys_name = ref.count("system") ? ref.at("system").string_value() : std::string();
  const std::string fac_name = ref.count("faction") ? ref.at("faction").string_value() : std::string();

  const auto& st = rr.sim.state();

  std::optional<Id> sys_id;
  if (!sys_name.empty()) {
    sys_id = resolve_system_by_name(rr, sys_name);
    if (!sys_id) {
      if (error) *error = "Unknown or undiscovered system '" + sys_name + "'";
      return std::nullopt;
    }
  }

  std::optional<Id> body_id;
  if (!body_name.empty()) {
    Object bref;
    bref["name"] = body_name;
    if (sys_id) bref["system"] = sys_name;
    body_id = resolve_body_by_ref(rr, bref, error);
    if (!body_id) return std::nullopt;
  }

  // Candidate filter: by body and/or by system visibility.
  std::vector<Id> candidates;
  for (const auto& [cid, c] : st.colonies) {
    const auto* b = find_ptr(st.bodies, c.body_id);
    if (b && !rr.allow_system(b->system_id)) continue;
    if (body_id && c.body_id != *body_id) continue;
    if (sys_id && b && b->system_id != *sys_id) continue;
    if (!fac_name.empty()) {
      const auto* f = find_ptr(st.factions, c.faction_id);
      if (!f || !eq_ci(f->name, fac_name)) continue;
    }
    candidates.push_back(c.id);
  }

  if (candidates.empty()) {
    if (error) *error = "Colony not found (body/system/faction filters removed all candidates)";
    return std::nullopt;
  }

  // If colony name is provided, match by name first.
  std::vector<Id> named;
  if (!colony_name.empty()) {
    for (Id cid : candidates) {
      if (const auto* c = find_ptr(st.colonies, cid)) {
        if (eq_ci(c->name, colony_name)) named.push_back(cid);
      }
    }
    if (named.size() == 1) return named.front();
    if (named.size() > 1) {
      if (error) *error = "Ambiguous colony '" + colony_name + "' (matches " + std::to_string(named.size()) + ")";
      return std::nullopt;
    }
  }

  // Fallback: if the filter narrowed to exactly one colony, accept it.
  if (candidates.size() == 1) return candidates.front();

  if (error) {
    if (!colony_name.empty()) {
      *error = "Colony '" + colony_name + "' not found (and multiple fallback candidates exist)";
    } else {
      *error = "Ambiguous colony ref (matches " + std::to_string(candidates.size()) + "). Add colony 'name'.";
    }
  }
  return std::nullopt;
}

std::optional<Id> resolve_jump_point_by_ref(const RefResolver& rr, const Object& ref, std::string* error) {
  const std::string name = ref.count("name") ? ref.at("name").string_value() : std::string();
  if (name.empty()) {
    if (error) *error = "jump_point_ref missing name";
    return std::nullopt;
  }

  const std::string sys_name = ref.count("system") ? ref.at("system").string_value() : std::string();
  const std::string dest_sys_name = ref.count("dest_system") ? ref.at("dest_system").string_value() : std::string();

  const auto& st = rr.sim.state();

  std::optional<Id> sys_id;
  if (!sys_name.empty()) {
    sys_id = resolve_system_by_name(rr, sys_name);
    if (!sys_id) {
      if (error) *error = "Unknown or undiscovered system '" + sys_name + "'";
      return std::nullopt;
    }
  }

  std::vector<Id> matches;
  for (const auto& [jid, jp] : st.jump_points) {
    if (!rr.allow_system(jp.system_id)) continue;
    if (sys_id && jp.system_id != *sys_id) continue;
    if (!eq_ci(jp.name, name)) continue;

    if (!dest_sys_name.empty() && jp.linked_jump_id != kInvalidId) {
      if (const auto* other = find_ptr(st.jump_points, jp.linked_jump_id)) {
        const auto* other_sys = find_ptr(st.systems, other->system_id);
        if (!other_sys || !eq_ci(other_sys->name, dest_sys_name)) continue;
      }
    }

    if (!rr.allow_jump_point(jp.id)) continue;
    matches.push_back(jp.id);
  }

  if (matches.empty()) {
    if (error) *error = "Jump point '" + name + "' not found (or not surveyed)";
    return std::nullopt;
  }
  if (matches.size() > 1) {
    if (error) {
      *error = "Ambiguous jump point '" + name + "' (matches " + std::to_string(matches.size()) +
               "). Add 'system' or 'dest_system'.";
    }
    return std::nullopt;
  }
  return matches.front();
}

std::optional<Id> resolve_anomaly_by_ref(const RefResolver& rr, const Object& ref, std::string* error) {
  const std::string name = ref.count("name") ? ref.at("name").string_value() : std::string();
  if (name.empty()) {
    if (error) *error = "anomaly_ref missing name";
    return std::nullopt;
  }
  const std::string sys_name = ref.count("system") ? ref.at("system").string_value() : std::string();

  const auto& st = rr.sim.state();
  std::optional<Id> sys_id;
  if (!sys_name.empty()) {
    sys_id = resolve_system_by_name(rr, sys_name);
    if (!sys_id) {
      if (error) *error = "Unknown or undiscovered system '" + sys_name + "'";
      return std::nullopt;
    }
  }

  std::vector<Id> matches;
  for (const auto& [aid, a] : st.anomalies) {
    if (!rr.allow_anomaly(a.id)) continue;
    if (!rr.allow_system(a.system_id)) continue;
    if (sys_id && a.system_id != *sys_id) continue;
    if (eq_ci(a.name, name)) matches.push_back(a.id);
  }
  if (matches.empty()) {
    if (error) *error = "Anomaly '" + name + "' not found (or not discovered)";
    return std::nullopt;
  }
  if (matches.size() > 1) {
    if (error) *error = "Ambiguous anomaly '" + name + "' (matches " + std::to_string(matches.size()) + ")";
    return std::nullopt;
  }
  return matches.front();
}

std::optional<Id> resolve_wreck_by_ref(const RefResolver& rr, const Object& ref, std::string* error) {
  const std::string name = ref.count("name") ? ref.at("name").string_value() : std::string();
  if (name.empty()) {
    if (error) *error = "wreck_ref missing name";
    return std::nullopt;
  }
  const std::string sys_name = ref.count("system") ? ref.at("system").string_value() : std::string();

  const auto& st = rr.sim.state();
  std::optional<Id> sys_id;
  if (!sys_name.empty()) {
    sys_id = resolve_system_by_name(rr, sys_name);
    if (!sys_id) {
      if (error) *error = "Unknown or undiscovered system '" + sys_name + "'";
      return std::nullopt;
    }
  }

  std::vector<Id> matches;
  for (const auto& [wid, w] : st.wrecks) {
    if (!rr.allow_system(w.system_id)) continue;
    if (sys_id && w.system_id != *sys_id) continue;
    if (eq_ci(w.name, name)) matches.push_back(w.id);
  }
  if (matches.empty()) {
    if (error) *error = "Wreck '" + name + "' not found";
    return std::nullopt;
  }
  if (matches.size() > 1) {
    if (error) *error = "Ambiguous wreck '" + name + "' (matches " + std::to_string(matches.size()) + ")";
    return std::nullopt;
  }
  return matches.front();
}

std::optional<Id> resolve_ship_by_ref(const RefResolver& rr, const Object& ref, std::string* error) {
  const std::string name = ref.count("name") ? ref.at("name").string_value() : std::string();
  if (name.empty()) {
    if (error) *error = "target_ship_ref missing name";
    return std::nullopt;
  }
  const std::string sys_name = ref.count("system") ? ref.at("system").string_value() : std::string();
  const std::string fac_name = ref.count("faction") ? ref.at("faction").string_value() : std::string();

  const auto& st = rr.sim.state();

  std::optional<Id> sys_id;
  if (!sys_name.empty()) {
    sys_id = resolve_system_by_name(rr, sys_name);
    if (!sys_id) {
      if (error) *error = "Unknown or undiscovered system '" + sys_name + "'";
      return std::nullopt;
    }
  }

  std::vector<Id> matches;
  for (const auto& [sid, sh] : st.ships) {
    if (!rr.allow_ship(sh.id)) continue;
    if (!rr.allow_system(sh.system_id)) continue;
    if (sys_id && sh.system_id != *sys_id) continue;
    if (!fac_name.empty()) {
      const auto* f = find_ptr(st.factions, sh.faction_id);
      if (!f || !eq_ci(f->name, fac_name)) continue;
    }
    if (eq_ci(sh.name, name)) matches.push_back(sh.id);
  }

  if (matches.empty()) {
    if (error) *error = "Ship '" + name + "' not found (or not detected)";
    return std::nullopt;
  }
  if (matches.size() > 1) {
    if (error) *error = "Ambiguous ship '" + name + "' (matches " + std::to_string(matches.size()) + ")";
    return std::nullopt;
  }
  return matches.front();
}

bool can_use_source_id_for_entity(const RefResolver& rr, const char* id_key, Id id) {
  const auto& st = rr.sim.state();
  const std::string k{id_key};

  if (k == "body_id") {
    if (const auto* b = find_ptr(st.bodies, id)) return rr.allow_system(b->system_id);
    return false;
  }
  if (k == "colony_id" || k == "dropoff_colony_id") {
    if (const auto* c = find_ptr(st.colonies, id)) {
      if (const auto* b = find_ptr(st.bodies, c->body_id)) return rr.allow_system(b->system_id);
      return true;
    }
    return false;
  }
  if (k == "jump_point_id") {
    if (const auto* jp = find_ptr(st.jump_points, id)) return rr.allow_system(jp->system_id) && rr.allow_jump_point(jp->id);
    return false;
  }
  if (k == "anomaly_id") {
    if (const auto* a = find_ptr(st.anomalies, id)) return rr.allow_anomaly(a->id) && rr.allow_system(a->system_id);
    return false;
  }
  if (k == "wreck_id") {
    if (const auto* w = find_ptr(st.wrecks, id)) return rr.allow_system(w->system_id);
    return false;
  }
  if (k == "target_ship_id") {
    if (const auto* sh = find_ptr(st.ships, id)) return rr.allow_ship(sh->id) && rr.allow_system(sh->system_id);
    return false;
  }
  if (k == "last_known_system_id") {
    return rr.allow_system(id);
  }

  return false;
}

bool resolve_id_field(const RefResolver& rr, Object& order_obj, const char* id_key, const char* ref_key,
                      std::optional<Id> (*resolve_fn)(const RefResolver&, const Object&, std::string*),
                      std::string* error) {
  // If ref exists, resolve and set id.
  if (auto it = order_obj.find(ref_key); it != order_obj.end() && it->second.is_object()) {
    std::string err;
    const auto id = resolve_fn(rr, it->second.object(), &err);
    if (!id) {
      if (error) *error = err.empty() ? (std::string("Failed to resolve ") + ref_key) : err;
      return false;
    }
    order_obj[id_key] = static_cast<double>(*id);
    return true;
  }

  // If already has the id, keep it.
  if (order_obj.find(id_key) != order_obj.end()) return true;

  // Fallback: accept source_*_id only when it resolves to a visible entity.
  const std::string source_key = std::string("source_") + id_key;
  if (auto it = order_obj.find(source_key); it != order_obj.end()) {
    const Id src = static_cast<Id>(it->second.int_value(kInvalidId));
    if (src != kInvalidId && can_use_source_id_for_entity(rr, id_key, src)) {
      order_obj[id_key] = static_cast<double>(src);
      return true;
    }
  }

  // No info to resolve; leave untouched (caller may not need this id).
  return true;
}

bool resolve_system_id_field(const RefResolver& rr, Object& order_obj, const char* id_key, const char* ref_key,
                             std::string* error) {
  if (auto it = order_obj.find(ref_key); it != order_obj.end() && it->second.is_object()) {
    const auto& ref = it->second.object();
    const std::string sys_name = ref.count("name") ? ref.at("name").string_value() : std::string();
    if (sys_name.empty()) {
      if (error) *error = std::string(ref_key) + " missing name";
      return false;
    }
    const auto sid = resolve_system_by_name(rr, sys_name);
    if (!sid) {
      if (error) *error = "Unknown or undiscovered system '" + sys_name + "'";
      return false;
    }
    order_obj[id_key] = static_cast<double>(*sid);
    return true;
  }

  if (order_obj.find(id_key) != order_obj.end()) return true;

  const std::string source_key = std::string("source_") + id_key;
  if (auto it = order_obj.find(source_key); it != order_obj.end()) {
    const Id src = static_cast<Id>(it->second.int_value(kInvalidId));
    if (src != kInvalidId && rr.allow_system(src)) {
      order_obj[id_key] = static_cast<double>(src);
      return true;
    }
  }

  return true;
}

bool seems_portable_template(const Value& v) {
  if (!v.is_object()) return false;
  const auto& obj = v.object();
  if (auto it = obj.find("portable"); it != obj.end() && it->second.bool_value(false)) return true;
  if (auto itv = obj.find("nebula4x_order_template_version"); itv != obj.end()) {
    if (itv->second.int_value(0) >= 2) return true;
  }
  if (auto ito = obj.find("orders"); ito != obj.end() && ito->second.is_array()) {
    for (const auto& ord_v : ito->second.array()) {
      if (!ord_v.is_object()) continue;
      const auto& o = ord_v.object();
      for (const auto& [k, _] : o) {
        if (k.size() >= 4 && k.substr(k.size() - 4) == "_ref") return true;
      }
    }
  }
  return false;
}

std::string norm_key(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (char raw : s) {
    const unsigned char uc = static_cast<unsigned char>(raw);
    if (std::isalnum(uc)) {
      out.push_back(static_cast<char>(std::tolower(uc)));
    }
  }
  return out;
}

bool name_matches(const std::string& candidate, const std::string& desired, bool allow_fuzzy) {
  if (desired.empty()) return false;
  if (eq_ci(candidate, desired)) return true;
  if (!allow_fuzzy) return false;
  return norm_key(candidate) == norm_key(desired);
}

std::vector<Id> systems_by_name(const RefResolver& rr, const std::string& name) {
  if (name.empty()) return {};
  const auto& st = rr.sim.state();

  std::vector<Id> exact;
  std::vector<Id> fuzzy;
  for (const auto& [sid, sys] : st.systems) {
    if (!rr.allow_system(sys.id)) continue;
    if (name_matches(sys.name, name, /*allow_fuzzy=*/false)) {
      exact.push_back(sys.id);
    } else if (name_matches(sys.name, name, /*allow_fuzzy=*/true)) {
      fuzzy.push_back(sys.id);
    }
  }
  return !exact.empty() ? exact : fuzzy;
}

std::vector<Id> factions_by_name(const RefResolver& rr, const std::string& name) {
  if (name.empty()) return {};
  const auto& st = rr.sim.state();
  std::vector<Id> exact;
  std::vector<Id> fuzzy;
  for (const auto& [fid, f] : st.factions) {
    (void)rr;
    if (name_matches(f.name, name, /*allow_fuzzy=*/false)) {
      exact.push_back(f.id);
    } else if (name_matches(f.name, name, /*allow_fuzzy=*/true)) {
      fuzzy.push_back(f.id);
    }
  }
  return !exact.empty() ? exact : fuzzy;
}

bool id_in(const std::vector<Id>& ids, Id id) {
  return std::find(ids.begin(), ids.end(), id) != ids.end();
}

std::string sys_label(const Simulation& sim, Id sys_id) {
  const auto& st = sim.state();
  const auto* sys = find_ptr(st.systems, sys_id);
  if (!sys) return "System #" + std::to_string(static_cast<unsigned long long>(sys_id));
  if (sys->name.empty()) return "System #" + std::to_string(static_cast<unsigned long long>(sys_id));
  return sys->name;
}

std::string body_label(const Simulation& sim, Id body_id) {
  const auto& st = sim.state();
  const auto* b = find_ptr(st.bodies, body_id);
  if (!b) return "Body #" + std::to_string(static_cast<unsigned long long>(body_id));
  const std::string nm = b->name.empty() ? ("Body #" + std::to_string(static_cast<unsigned long long>(body_id))) : b->name;
  return nm + " — " + sys_label(sim, b->system_id);
}

std::string colony_label(const Simulation& sim, Id colony_id) {
  const auto& st = sim.state();
  const auto* c = find_ptr(st.colonies, colony_id);
  if (!c) return "Colony #" + std::to_string(static_cast<unsigned long long>(colony_id));

  const auto* b = find_ptr(st.bodies, c->body_id);
  const auto* f = find_ptr(st.factions, c->faction_id);

  const std::string cn = c->name.empty() ? ("Colony #" + std::to_string(static_cast<unsigned long long>(colony_id))) : c->name;
  const std::string bn = (b && !b->name.empty()) ? b->name : "(unknown body)";
  const std::string sn = (b ? sys_label(sim, b->system_id) : "(unknown system)");
  const std::string fn = (f && !f->name.empty()) ? f->name : "(unknown faction)";

  return cn + " — " + bn + " (" + sn + ") — " + fn;
}

std::string jump_point_label(const Simulation& sim, Id jump_point_id) {
  const auto& st = sim.state();
  const auto* jp = find_ptr(st.jump_points, jump_point_id);
  if (!jp) return "JumpPoint #" + std::to_string(static_cast<unsigned long long>(jump_point_id));

  const std::string jn = jp->name.empty() ? ("JumpPoint #" + std::to_string(static_cast<unsigned long long>(jump_point_id))) : jp->name;
  const std::string sn = sys_label(sim, jp->system_id);

  std::string dest;
  if (jp->linked_jump_id != kInvalidId) {
    if (const auto* other = find_ptr(st.jump_points, jp->linked_jump_id)) {
      dest = sys_label(sim, other->system_id);
    }
  }
  if (!dest.empty()) return jn + " — " + sn + " -> " + dest;
  return jn + " — " + sn;
}

std::string ship_label(const Simulation& sim, Id ship_id) {
  const auto& st = sim.state();
  const auto* sh = find_ptr(st.ships, ship_id);
  if (!sh) return "Ship #" + std::to_string(static_cast<unsigned long long>(ship_id));
  const auto* f = find_ptr(st.factions, sh->faction_id);
  const std::string sn = sys_label(sim, sh->system_id);
  const std::string fn = (f && !f->name.empty()) ? f->name : "(unknown faction)";
  const std::string nm = sh->name.empty() ? ("Ship #" + std::to_string(static_cast<unsigned long long>(ship_id))) : sh->name;
  return nm + " — " + sn + " — " + fn;
}

std::string anomaly_label(const Simulation& sim, Id anomaly_id) {
  const auto& st = sim.state();
  const auto* a = find_ptr(st.anomalies, anomaly_id);
  if (!a) return "Anomaly #" + std::to_string(static_cast<unsigned long long>(anomaly_id));
  const std::string nm = a->name.empty() ? ("Anomaly #" + std::to_string(static_cast<unsigned long long>(anomaly_id))) : a->name;
  return nm + " — " + sys_label(sim, a->system_id);
}

std::string wreck_label(const Simulation& sim, Id wreck_id) {
  const auto& st = sim.state();
  const auto* w = find_ptr(st.wrecks, wreck_id);
  if (!w) return "Wreck #" + std::to_string(static_cast<unsigned long long>(wreck_id));
  const std::string nm = w->name.empty() ? ("Wreck #" + std::to_string(static_cast<unsigned long long>(wreck_id))) : w->name;
  return nm + " — " + sys_label(sim, w->system_id);
}

std::string system_ref_label(const Simulation& sim, Id sys_id) {
  return sys_label(sim, sys_id);
}

std::string ref_summary_body(const Object& ref) {
  const std::string nm = ref.count("name") ? ref.at("name").string_value() : std::string();
  const std::string sys = ref.count("system") ? ref.at("system").string_value() : std::string();
  if (!sys.empty()) return nm + " (" + sys + ")";
  return nm;
}

std::string ref_summary_colony(const Object& ref) {
  const std::string cn = ref.count("name") ? ref.at("name").string_value() : std::string();
  const std::string bn = ref.count("body") ? ref.at("body").string_value() : std::string();
  const std::string sys = ref.count("system") ? ref.at("system").string_value() : std::string();
  const std::string fac = ref.count("faction") ? ref.at("faction").string_value() : std::string();

  std::string out = cn;
  if (!bn.empty() || !sys.empty()) {
    if (!out.empty()) out += " — ";
    out += bn;
    if (!sys.empty()) out += " (" + sys + ")";
  }
  if (!fac.empty()) {
    if (!out.empty()) out += " — ";
    out += fac;
  }
  return out;
}

std::string ref_summary_jump_point(const Object& ref) {
  const std::string nm = ref.count("name") ? ref.at("name").string_value() : std::string();
  const std::string sys = ref.count("system") ? ref.at("system").string_value() : std::string();
  const std::string dst = ref.count("dest_system") ? ref.at("dest_system").string_value() : std::string();
  std::string out = nm;
  if (!sys.empty()) {
    out += " (" + sys;
    if (!dst.empty()) out += " -> " + dst;
    out += ")";
  }
  return out;
}

std::string ref_summary_ship(const Object& ref) {
  const std::string nm = ref.count("name") ? ref.at("name").string_value() : std::string();
  const std::string sys = ref.count("system") ? ref.at("system").string_value() : std::string();
  const std::string fac = ref.count("faction") ? ref.at("faction").string_value() : std::string();
  std::string out = nm;
  if (!fac.empty() || !sys.empty()) {
    out += " (";
    if (!fac.empty()) out += fac;
    if (!fac.empty() && !sys.empty()) out += " — ";
    if (!sys.empty()) out += sys;
    out += ")";
  }
  return out;
}

std::string ref_summary_anomaly_or_wreck(const Object& ref) {
  const std::string nm = ref.count("name") ? ref.at("name").string_value() : std::string();
  const std::string sys = ref.count("system") ? ref.at("system").string_value() : std::string();
  if (!sys.empty()) return nm + " (" + sys + ")";
  return nm;
}

std::string ref_summary_system(const Object& ref) {
  const std::string nm = ref.count("name") ? ref.at("name").string_value() : std::string();
  return nm;
}

template <typename LabelFn>
std::vector<PortableTemplateRefCandidate> ids_to_candidates(const Simulation& sim, const std::vector<Id>& ids,
                                                           LabelFn label_fn) {
  std::vector<PortableTemplateRefCandidate> out;
  out.reserve(ids.size());
  for (Id id : ids) {
    PortableTemplateRefCandidate c;
    c.id = id;
    c.label = label_fn(sim, id);
    out.push_back(std::move(c));
  }
  return out;
}

std::vector<Id> body_candidates_for_ref(const RefResolver& rr, const Object& ref) {
  const std::string name = ref.count("name") ? ref.at("name").string_value() : std::string();
  if (name.empty()) return {};
  const std::string sys_name = ref.count("system") ? ref.at("system").string_value() : std::string();
  const std::vector<Id> sys_ids = systems_by_name(rr, sys_name);

  const auto& st = rr.sim.state();
  std::vector<Id> exact;
  std::vector<Id> fuzzy;
  for (const auto& [bid, b] : st.bodies) {
    if (!rr.allow_system(b.system_id)) continue;
    if (!sys_name.empty() && !id_in(sys_ids, b.system_id)) continue;

    if (name_matches(b.name, name, /*allow_fuzzy=*/false)) {
      exact.push_back(b.id);
    } else if (name_matches(b.name, name, /*allow_fuzzy=*/true)) {
      fuzzy.push_back(b.id);
    }
  }
  return !exact.empty() ? exact : fuzzy;
}

std::vector<Id> colony_candidates_for_ref(const RefResolver& rr, const Object& ref) {
  const std::string colony_name = ref.count("name") ? ref.at("name").string_value() : std::string();
  const std::string body_name = ref.count("body") ? ref.at("body").string_value() : std::string();
  const std::string sys_name = ref.count("system") ? ref.at("system").string_value() : std::string();
  const std::string fac_name = ref.count("faction") ? ref.at("faction").string_value() : std::string();

  const std::vector<Id> sys_ids = systems_by_name(rr, sys_name);
  const std::vector<Id> fac_ids = factions_by_name(rr, fac_name);

  std::vector<Id> body_ids;
  if (!body_name.empty()) {
    Object bref;
    bref["name"] = body_name;
    if (!sys_name.empty()) bref["system"] = sys_name;
    body_ids = body_candidates_for_ref(rr, bref);
  }

  const auto& st = rr.sim.state();
  std::vector<Id> candidates;
  candidates.reserve(st.colonies.size());

  for (const auto& [cid, c] : st.colonies) {
    const auto* b = find_ptr(st.bodies, c.body_id);
    if (b && !rr.allow_system(b->system_id)) continue;
    if (!sys_name.empty() && (!b || !id_in(sys_ids, b->system_id))) continue;
    if (!body_name.empty() && !id_in(body_ids, c.body_id)) continue;
    if (!fac_name.empty() && !id_in(fac_ids, c.faction_id)) continue;
    candidates.push_back(c.id);
  }

  if (colony_name.empty()) return candidates;

  std::vector<Id> exact;
  std::vector<Id> fuzzy;
  for (Id id : candidates) {
    if (const auto* c = find_ptr(st.colonies, id)) {
      if (name_matches(c->name, colony_name, /*allow_fuzzy=*/false)) {
        exact.push_back(id);
      } else if (name_matches(c->name, colony_name, /*allow_fuzzy=*/true)) {
        fuzzy.push_back(id);
      }
    }
  }
  return !exact.empty() ? exact : fuzzy;
}

std::vector<Id> jump_point_candidates_for_ref(const RefResolver& rr, const Object& ref) {
  const std::string name = ref.count("name") ? ref.at("name").string_value() : std::string();
  if (name.empty()) return {};
  const std::string sys_name = ref.count("system") ? ref.at("system").string_value() : std::string();
  const std::string dst_name = ref.count("dest_system") ? ref.at("dest_system").string_value() : std::string();

  const std::vector<Id> sys_ids = systems_by_name(rr, sys_name);

  const auto& st = rr.sim.state();

  std::vector<Id> exact;
  std::vector<Id> fuzzy;
  for (const auto& [jid, jp] : st.jump_points) {
    if (!rr.allow_system(jp.system_id)) continue;
    if (!rr.allow_jump_point(jp.id)) continue;
    if (!sys_name.empty() && !id_in(sys_ids, jp.system_id)) continue;

    if (!dst_name.empty()) {
      if (jp.linked_jump_id == kInvalidId) continue;
      const auto* other = find_ptr(st.jump_points, jp.linked_jump_id);
      if (!other) continue;
      const std::string other_sys = system_name_if_visible(rr, other->system_id);
      if (!name_matches(other_sys, dst_name, /*allow_fuzzy=*/true)) continue;
    }

    if (name_matches(jp.name, name, /*allow_fuzzy=*/false)) {
      exact.push_back(jp.id);
    } else if (name_matches(jp.name, name, /*allow_fuzzy=*/true)) {
      fuzzy.push_back(jp.id);
    }
  }
  return !exact.empty() ? exact : fuzzy;
}

std::vector<Id> ship_candidates_for_ref(const RefResolver& rr, const Object& ref) {
  const std::string name = ref.count("name") ? ref.at("name").string_value() : std::string();
  if (name.empty()) return {};
  const std::string sys_name = ref.count("system") ? ref.at("system").string_value() : std::string();
  const std::string fac_name = ref.count("faction") ? ref.at("faction").string_value() : std::string();

  const std::vector<Id> sys_ids = systems_by_name(rr, sys_name);
  const std::vector<Id> fac_ids = factions_by_name(rr, fac_name);

  const auto& st = rr.sim.state();
  std::vector<Id> exact;
  std::vector<Id> fuzzy;
  for (const auto& [sid, sh] : st.ships) {
    if (!rr.allow_ship(sh.id)) continue;
    if (!rr.allow_system(sh.system_id)) continue;
    if (!sys_name.empty() && !id_in(sys_ids, sh.system_id)) continue;
    if (!fac_name.empty() && !id_in(fac_ids, sh.faction_id)) continue;

    if (name_matches(sh.name, name, /*allow_fuzzy=*/false)) {
      exact.push_back(sh.id);
    } else if (name_matches(sh.name, name, /*allow_fuzzy=*/true)) {
      fuzzy.push_back(sh.id);
    }
  }
  return !exact.empty() ? exact : fuzzy;
}

std::vector<Id> anomaly_candidates_for_ref(const RefResolver& rr, const Object& ref) {
  const std::string name = ref.count("name") ? ref.at("name").string_value() : std::string();
  if (name.empty()) return {};
  const std::string sys_name = ref.count("system") ? ref.at("system").string_value() : std::string();
  const std::vector<Id> sys_ids = systems_by_name(rr, sys_name);

  const auto& st = rr.sim.state();
  std::vector<Id> exact;
  std::vector<Id> fuzzy;
  for (const auto& [aid, a] : st.anomalies) {
    if (!rr.allow_anomaly(a.id)) continue;
    if (!rr.allow_system(a.system_id)) continue;
    if (!sys_name.empty() && !id_in(sys_ids, a.system_id)) continue;

    if (name_matches(a.name, name, /*allow_fuzzy=*/false)) {
      exact.push_back(a.id);
    } else if (name_matches(a.name, name, /*allow_fuzzy=*/true)) {
      fuzzy.push_back(a.id);
    }
  }
  return !exact.empty() ? exact : fuzzy;
}

std::vector<Id> wreck_candidates_for_ref(const RefResolver& rr, const Object& ref) {
  const std::string name = ref.count("name") ? ref.at("name").string_value() : std::string();
  if (name.empty()) return {};
  const std::string sys_name = ref.count("system") ? ref.at("system").string_value() : std::string();
  const std::vector<Id> sys_ids = systems_by_name(rr, sys_name);

  const auto& st = rr.sim.state();
  std::vector<Id> exact;
  std::vector<Id> fuzzy;
  for (const auto& [wid, w] : st.wrecks) {
    if (!rr.allow_system(w.system_id)) continue;
    if (!sys_name.empty() && !id_in(sys_ids, w.system_id)) continue;

    if (name_matches(w.name, name, /*allow_fuzzy=*/false)) {
      exact.push_back(w.id);
    } else if (name_matches(w.name, name, /*allow_fuzzy=*/true)) {
      fuzzy.push_back(w.id);
    }
  }
  return !exact.empty() ? exact : fuzzy;
}

std::vector<Id> system_candidates_for_ref(const RefResolver& rr, const Object& ref) {
  const std::string name = ref.count("name") ? ref.at("name").string_value() : std::string();
  return systems_by_name(rr, name);
}

void add_issue(PortableTemplateImportSession* sess, int order_index, const std::string& order_type,
               const std::string& id_key, const std::string& ref_key, const std::string& ref_summary,
               const std::string& message, std::vector<PortableTemplateRefCandidate> candidates) {
  if (!sess) return;
  PortableTemplateImportIssue issue;
  issue.order_index = order_index;
  issue.order_type = order_type;
  issue.id_key = id_key;
  issue.ref_key = ref_key;
  issue.ref_summary = ref_summary;
  issue.message = message;
  issue.candidates = std::move(candidates);
  issue.selected_candidate = -1;
  sess->issues.push_back(std::move(issue));
}

// Resolve a single ref field in a JSON order object, collecting issues rather than failing.
// Returns true if the field is either not present, already resolved, or successfully auto-resolved.
// Returns false only for structural JSON corruption.
template <typename CandidateFn, typename LabelFn>
bool resolve_or_collect_issue(const RefResolver& rr, PortableTemplateImportSession* sess, Object& order_obj,
                              int order_index, const std::string& order_type, const char* id_key, const char* ref_key,
                              CandidateFn cand_fn, LabelFn label_fn, const char* ref_kind,
                              std::string (*ref_summary_fn)(const Object&)) {
  // Only act when the portable ref is present.
  const auto it_ref = order_obj.find(ref_key);
  if (it_ref == order_obj.end()) return true;

  // If id already exists, assume resolved.
  if (order_obj.find(id_key) != order_obj.end()) return true;

  if (!it_ref->second.is_object()) {
    add_issue(sess, order_index, order_type, id_key, ref_key, std::string(ref_kind) + " (invalid ref)",
              std::string(ref_key) + " is not an object", {});
    return true;
  }

  const Object& ref = it_ref->second.object();
  const std::vector<Id> ids = cand_fn(rr, ref);

  // source_*_id fallback (same-save copy/paste).
  const std::string source_key = std::string("source_") + id_key;
  const auto it_src = order_obj.find(source_key);
  const Id src_id = (it_src != order_obj.end()) ? static_cast<Id>(it_src->second.int_value(kInvalidId)) : kInvalidId;
  const bool src_usable = (src_id != kInvalidId) && can_use_source_id_for_entity(rr, id_key, src_id);

  if (ids.size() == 1) {
    order_obj[id_key] = static_cast<double>(ids.front());
    return true;
  }

  if (ids.empty()) {
    if (src_usable) {
      order_obj[id_key] = static_cast<double>(src_id);
      return true;
    }
    const std::string summary = ref_summary_fn ? ref_summary_fn(ref) : std::string(ref_kind);
    add_issue(sess, order_index, order_type, id_key, ref_key, summary,
              "No matching entities found in this save (or blocked by fog-of-war).", {});
    return true;
  }

  // ids.size() > 1
  if (src_usable && id_in(ids, src_id)) {
    // Same-save paste: keep the original id.
    order_obj[id_key] = static_cast<double>(src_id);
    return true;
  }

  auto cands = ids_to_candidates(rr.sim, ids, label_fn);
  std::string msg = "Ambiguous reference (" + std::to_string(ids.size()) + " matches). Select one.";
  if (src_id != kInvalidId && !src_usable) {
    msg += " (source id not usable under fog-of-war)";
  }
  const std::string summary = ref_summary_fn ? ref_summary_fn(ref) : std::string(ref_kind);
  add_issue(sess, order_index, order_type, id_key, ref_key, summary, msg, std::move(cands));
  return true;
}

}  // namespace

std::string serialize_order_template_to_json_portable(const Simulation& sim, const std::string& name,
                                                     const std::vector<Order>& orders,
                                                     const PortableOrderTemplateOptions& opts,
                                                     int indent) {
  std::vector<Order> filtered = orders;
  if (opts.strip_travel_via_jump) {
    filtered.clear();
    filtered.reserve(orders.size());
    for (const auto& o : orders) {
      if (!std::holds_alternative<TravelViaJump>(o)) filtered.push_back(o);
    }
  }

  // Start from the canonical v1 template JSON and then enrich it.
  Value root = nebula4x::serialize_order_template_to_json_value(name, filtered, /*template_format_version=*/1);

  auto* robj = root.as_object();
  if (!robj) return nebula4x::serialize_order_template_to_json(name, filtered, indent);

  (*robj)["nebula4x_order_template_version"] = static_cast<double>(2);
  (*robj)["portable"] = true;

  RefResolver rr{sim, opts.viewer_faction_id, opts.fog_of_war};

  Value& orders_v = (*robj)["orders"];
  auto* arr = orders_v.as_array();
  if (!arr) return nebula4x::json::stringify(root, indent);

  for (auto& ov : *arr) {
    auto* o = ov.as_object();
    if (!o) continue;

    // Body targets.
    if (auto it = o->find("body_id"); it != o->end()) {
      const Id bid = static_cast<Id>(it->second.int_value(kInvalidId));
      add_portable_ref(*o, "body_id", "body_ref", make_body_ref(rr, bid), opts.include_source_ids);
    }

    // Colony targets.
    if (auto it = o->find("colony_id"); it != o->end()) {
      const Id cid = static_cast<Id>(it->second.int_value(kInvalidId));
      add_portable_ref(*o, "colony_id", "colony_ref", make_colony_ref(rr, cid), opts.include_source_ids);
    }
    if (auto it = o->find("dropoff_colony_id"); it != o->end()) {
      const Id cid = static_cast<Id>(it->second.int_value(kInvalidId));
      add_portable_ref(*o, "dropoff_colony_id", "dropoff_colony_ref", make_colony_ref(rr, cid), opts.include_source_ids);
    }

    // Jump points.
    if (auto it = o->find("jump_point_id"); it != o->end()) {
      const Id jid = static_cast<Id>(it->second.int_value(kInvalidId));
      add_portable_ref(*o, "jump_point_id", "jump_point_ref", make_jump_point_ref(rr, jid), opts.include_source_ids);
    }

    // Ships.
    if (auto it = o->find("target_ship_id"); it != o->end()) {
      const Id sid = static_cast<Id>(it->second.int_value(kInvalidId));
      add_portable_ref(*o, "target_ship_id", "target_ship_ref", make_ship_ref(rr, sid), opts.include_source_ids);
    }

    // Optional last-known system references (for attack_ship).
    if (auto it = o->find("last_known_system_id"); it != o->end()) {
      const Id sys = static_cast<Id>(it->second.int_value(kInvalidId));
      const Object sref = make_system_ref(rr, sys);
      if (ref_has_name(sref)) {
        if (opts.include_source_ids) (*o)[std::string("source_") + "last_known_system_id"] = it->second;
        o->erase(it);
        (*o)["last_known_system_ref"] = sref;
      }
    }

    // Wrecks.
    if (auto it = o->find("wreck_id"); it != o->end()) {
      const Id wid = static_cast<Id>(it->second.int_value(kInvalidId));
      add_portable_ref(*o, "wreck_id", "wreck_ref", make_wreck_ref(rr, wid), opts.include_source_ids);
    }

    // Anomalies.
    if (auto it = o->find("anomaly_id"); it != o->end()) {
      const Id aid = static_cast<Id>(it->second.int_value(kInvalidId));
      add_portable_ref(*o, "anomaly_id", "anomaly_ref", make_anomaly_ref(rr, aid), opts.include_source_ids);
    }
  }

  return nebula4x::json::stringify(root, indent);
}

bool deserialize_order_template_from_json_portable(const Simulation& sim, Id viewer_faction_id, bool fog_of_war,
                                                  const std::string& json_text, ParsedOrderTemplate* out,
                                                  std::string* error) {
  auto fail = [&](const std::string& msg) {
    if (error) *error = msg;
    return false;
  };

  if (!out) return fail("Output pointer is null");

  // First, attempt to parse. We may fall back to the legacy parser for non-portable docs.
  Value v;
  try {
    v = nebula4x::json::parse(json_text);
  } catch (const std::exception& e) {
    return fail(std::string("JSON parse failed: ") + e.what());
  }

  if (!seems_portable_template(v)) {
    // Delegate to canonical parser for legacy templates (ID-based) or raw order arrays.
    return nebula4x::deserialize_order_template_from_json(json_text, out, error);
  }

  // Normalize to an object with an 'orders' array.
  Value root = v;
  if (root.is_array()) {
    Object o;
    o["nebula4x_order_template_version"] = static_cast<double>(2);
    o["portable"] = true;
    o["orders"] = root;
    root = o;
  }

  if (!root.is_object()) return fail("Expected a JSON object or array");

  auto* robj = root.as_object();
  if (!robj) return fail("Internal error: object cast failed");

  Value* orders_v = nullptr;
  if (auto it = robj->find("orders"); it != robj->end()) {
    orders_v = &it->second;
  } else if (auto it_queue = robj->find("queue"); it_queue != robj->end()) {
    // Accept ship-orders shape.
    orders_v = &it_queue->second;
  } else {
    return fail("Missing 'orders' array");
  }

  if (!orders_v || !orders_v->is_array()) return fail("'orders' is not an array");

  RefResolver rr{sim, viewer_faction_id, fog_of_war};

  // Resolve references by mutating a copy of the JSON into a legacy, id-filled template.
  for (auto& ov : *orders_v->as_array()) {
    auto* o = ov.as_object();
    if (!o) continue;

    std::string err;

    if (!resolve_id_field(rr, *o, "body_id", "body_ref", resolve_body_by_ref, &err)) return fail(err);
    if (!resolve_id_field(rr, *o, "colony_id", "colony_ref", resolve_colony_by_ref, &err)) return fail(err);
    if (!resolve_id_field(rr, *o, "dropoff_colony_id", "dropoff_colony_ref", resolve_colony_by_ref, &err)) return fail(err);

    if (!resolve_id_field(rr, *o, "jump_point_id", "jump_point_ref", resolve_jump_point_by_ref, &err)) return fail(err);
    if (!resolve_id_field(rr, *o, "target_ship_id", "target_ship_ref", resolve_ship_by_ref, &err)) return fail(err);

    if (!resolve_id_field(rr, *o, "anomaly_id", "anomaly_ref", resolve_anomaly_by_ref, &err)) return fail(err);
    if (!resolve_id_field(rr, *o, "wreck_id", "wreck_ref", resolve_wreck_by_ref, &err)) return fail(err);

    if (!resolve_system_id_field(rr, *o, "last_known_system_id", "last_known_system_ref", &err)) return fail(err);
  }

  const std::string resolved_text = nebula4x::json::stringify(root, 2);
  return nebula4x::deserialize_order_template_from_json(resolved_text, out, error);
}

bool start_portable_template_import_session(const Simulation& sim, Id viewer_faction_id, bool fog_of_war,
                                            const std::string& json_text, PortableTemplateImportSession* out_session,
                                            std::string* error) {
  auto fail = [&](const std::string& msg) {
    if (error) *error = msg;
    return false;
  };

  if (!out_session) return fail("Output pointer is null");
  out_session->template_name.clear();
  out_session->root = Value{};
  out_session->issues.clear();
  out_session->total_orders = 0;

  Value v;
  try {
    v = nebula4x::json::parse(json_text);
  } catch (const std::exception& e) {
    return fail(std::string("JSON parse failed: ") + e.what());
  }

  // Normalize to an object with an 'orders' array.
  Value root = v;
  if (root.is_array()) {
    Object o;
    o["nebula4x_order_template_version"] = static_cast<double>(2);
    o["portable"] = seems_portable_template(v);
    o["orders"] = root;
    root = o;
  } else if (root.is_object()) {
    Object o = root.object();
    if (o.find("orders") == o.end()) {
      if (auto itq = o.find("queue"); itq != o.end() && itq->second.is_array()) {
        // Accept ship-orders shape; normalize to orders for in-place mutation.
        o["orders"] = itq->second;
      }
    }
    root = o;
  } else {
    return fail("Expected a JSON object or array");
  }

  auto* robj = root.as_object();
  if (!robj) return fail("Internal error: object cast failed");

  if (auto itn = robj->find("name"); itn != robj->end()) {
    out_session->template_name = itn->second.string_value();
  }

  Value* orders_v = nullptr;
  if (auto ito = robj->find("orders"); ito != robj->end()) {
    orders_v = &ito->second;
  }
  if (!orders_v || !orders_v->is_array()) {
    return fail("Missing 'orders' array");
  }

  out_session->root = root;
  out_session->total_orders = static_cast<int>(orders_v->array().size());

  RefResolver rr{sim, viewer_faction_id, fog_of_war};

  // Resolve what we can, but collect issues instead of failing.
  auto* out_obj = out_session->root.as_object();
  if (!out_obj) return fail("Internal error: session object cast failed");
  auto ito = out_obj->find("orders");
  if (ito == out_obj->end() || !ito->second.is_array()) return fail("Internal error: session orders missing");
  auto* arr = ito->second.as_array();
  if (!arr) return fail("Internal error: session orders cast failed");

  for (int i = 0; i < static_cast<int>(arr->size()); ++i) {
    auto* o = (*arr)[static_cast<std::size_t>(i)].as_object();
    if (!o) continue;
    const std::string order_type = o->count("type") ? o->at("type").string_value() : std::string("(unknown)");

    if (!resolve_or_collect_issue(rr, out_session, *o, i, order_type, "body_id", "body_ref", body_candidates_for_ref,
                                 body_label, "body", ref_summary_body)) {
      return fail("Internal error while resolving body_ref");
    }

    if (!resolve_or_collect_issue(rr, out_session, *o, i, order_type, "colony_id", "colony_ref",
                                 colony_candidates_for_ref, colony_label, "colony", ref_summary_colony)) {
      return fail("Internal error while resolving colony_ref");
    }
    if (!resolve_or_collect_issue(rr, out_session, *o, i, order_type, "dropoff_colony_id", "dropoff_colony_ref",
                                 colony_candidates_for_ref, colony_label, "colony", ref_summary_colony)) {
      return fail("Internal error while resolving dropoff_colony_ref");
    }

    if (!resolve_or_collect_issue(rr, out_session, *o, i, order_type, "jump_point_id", "jump_point_ref",
                                 jump_point_candidates_for_ref, jump_point_label, "jump_point",
                                 ref_summary_jump_point)) {
      return fail("Internal error while resolving jump_point_ref");
    }

    if (!resolve_or_collect_issue(rr, out_session, *o, i, order_type, "target_ship_id", "target_ship_ref",
                                 ship_candidates_for_ref, ship_label, "ship", ref_summary_ship)) {
      return fail("Internal error while resolving target_ship_ref");
    }

    if (!resolve_or_collect_issue(rr, out_session, *o, i, order_type, "anomaly_id", "anomaly_ref",
                                 anomaly_candidates_for_ref, anomaly_label, "anomaly", ref_summary_anomaly_or_wreck)) {
      return fail("Internal error while resolving anomaly_ref");
    }
    if (!resolve_or_collect_issue(rr, out_session, *o, i, order_type, "wreck_id", "wreck_ref", wreck_candidates_for_ref,
                                 wreck_label, "wreck", ref_summary_anomaly_or_wreck)) {
      return fail("Internal error while resolving wreck_ref");
    }

    if (!resolve_or_collect_issue(rr, out_session, *o, i, order_type, "last_known_system_id", "last_known_system_ref",
                                 system_candidates_for_ref, system_ref_label, "system", ref_summary_system)) {
      return fail("Internal error while resolving last_known_system_ref");
    }
  }

  return true;
}

bool finalize_portable_template_import_session(const Simulation& sim, PortableTemplateImportSession* session,
                                               ParsedOrderTemplate* out, std::string* error) {
  (void)sim;
  auto fail = [&](const std::string& msg) {
    if (error) *error = msg;
    return false;
  };

  if (!session) return fail("Session pointer is null");
  if (!out) return fail("Output pointer is null");

  auto* robj = session->root.as_object();
  if (!robj) return fail("Session root is not an object");
  auto it = robj->find("orders");
  if (it == robj->end() || !it->second.is_array()) return fail("Session missing 'orders' array");
  auto* arr = it->second.as_array();
  if (!arr) return fail("Internal error: orders array cast failed");

  // Validate all issues have a selection.
  for (const auto& iss : session->issues) {
    if (iss.order_index < 0 || iss.order_index >= static_cast<int>(arr->size())) {
      return fail("Issue references out-of-range order index");
    }
    if (iss.candidates.empty()) {
      return fail("Unresolvable reference in order #" + std::to_string(iss.order_index + 1) + ": " + iss.ref_summary);
    }
    if (iss.selected_candidate < 0 || iss.selected_candidate >= static_cast<int>(iss.candidates.size())) {
      return fail("Unresolved reference in order #" + std::to_string(iss.order_index + 1) + ": " + iss.ref_summary);
    }
  }

  // Apply selections.
  for (const auto& iss : session->issues) {
    auto* o = (*arr)[static_cast<std::size_t>(iss.order_index)].as_object();
    if (!o) return fail("Internal error: order is not an object");
    const Id chosen = iss.candidates[static_cast<std::size_t>(iss.selected_candidate)].id;
    if (chosen == kInvalidId) {
      return fail("Internal error: selected candidate has invalid id");
    }
    (*o)[iss.id_key] = static_cast<double>(chosen);
  }

  const std::string resolved_text = nebula4x::json::stringify(session->root, 2);
  return nebula4x::deserialize_order_template_from_json(resolved_text, out, error);
}

}  // namespace nebula4x::ui
