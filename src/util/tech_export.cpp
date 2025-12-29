#include "nebula4x/util/tech_export.h"

#include <algorithm>
#include <cstddef>
#include <sstream>
#include <string>
#include <vector>

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

std::string escape_dot(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"': out += "\\\""; break;
      case '\n': out += "\\n"; break;
      case '\r': break;
      default: out.push_back(c); break;
    }
  }
  return out;
}

} // namespace

std::string tech_tree_to_json(const std::unordered_map<std::string, TechDef>& techs) {
  json::Array out;
  out.reserve(techs.size());

  for (const auto& id : sorted_keys(techs)) {
    const auto it = techs.find(id);
    if (it == techs.end()) continue;
    const TechDef& t = it->second;

    json::Object obj;
    obj["id"] = t.id;
    obj["name"] = t.name;
    obj["cost"] = t.cost;

    // Prereqs
    json::Array prereqs;
    prereqs.reserve(t.prereqs.size());
    for (const auto& p : t.prereqs) prereqs.push_back(p);
    obj["prereqs"] = json::array(std::move(prereqs));

    // Effects
    json::Array effects;
    effects.reserve(t.effects.size());
    for (const auto& e : t.effects) {
      json::Object eo;
      eo["type"] = e.type;
      eo["value"] = e.value;
      if (e.amount != 0.0) eo["amount"] = e.amount;
      effects.push_back(json::object(std::move(eo)));
    }
    obj["effects"] = json::array(std::move(effects));

    out.push_back(json::object(std::move(obj)));
  }

  return json::stringify(json::array(std::move(out))) + "\n";
}

std::string tech_tree_to_dot(const std::unordered_map<std::string, TechDef>& techs) {
  std::ostringstream ss;
  ss << "digraph TechTree {\n";
  ss << "  rankdir=LR;\n";
  ss << "  node [shape=box];\n";

  // Nodes
  for (const auto& id : sorted_keys(techs)) {
    const auto it = techs.find(id);
    if (it == techs.end()) continue;
    const TechDef& t = it->second;

    std::string label = t.name;
    if (!t.name.empty()) label += "\\n";
    label += "(" + t.id + ")";
    if (t.cost > 0.0) {
      label += "\\nCost: " + std::to_string(static_cast<int>(t.cost));
    }

    ss << "  \"" << escape_dot(t.id) << "\" [label=\"" << escape_dot(label) << "\"];\n";
  }

  // Edges
  for (const auto& id : sorted_keys(techs)) {
    const auto it = techs.find(id);
    if (it == techs.end()) continue;
    const TechDef& t = it->second;
    for (const auto& pre : t.prereqs) {
      if (pre.empty()) continue;
      ss << "  \"" << escape_dot(pre) << "\" -> \"" << escape_dot(t.id) << "\";\n";
    }
  }

  ss << "}\n";
  return ss.str();
}

} // namespace nebula4x
