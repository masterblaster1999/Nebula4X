#include "nebula4x/core/content_validation.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace nebula4x {

namespace {

void push(std::vector<std::string>& out, std::string msg) { out.push_back(std::move(msg)); }

template <typename... Parts>
std::string join(Parts&&... parts) {
  std::ostringstream ss;
  (ss << ... << std::forward<Parts>(parts));
  return ss.str();
}

bool is_non_negative(double v) { return v >= 0.0 && std::isfinite(v); }

} // namespace

std::vector<std::string> validate_content_db(const ContentDB& db) {
  std::vector<std::string> errors;


  const auto resource_known = [&](const std::string& rid) -> bool {
    if (db.resources.empty()) return true;
    return db.resources.find(rid) != db.resources.end();
  };

  // --- Resources (optional) ---
  for (const auto& [key, r] : db.resources) {
    if (key.empty()) push(errors, "Resource map contains an empty key");
    if (r.id.empty()) push(errors, join("Resource '", key, "' has an empty id field"));
    if (!r.id.empty() && !key.empty() && r.id != key)
      push(errors, join("Resource key/id mismatch: key '", key, "' != id '", r.id, "'"));
    if (r.name.empty()) push(errors, join("Resource '", key, "' has an empty name"));
    if (r.category.empty()) push(errors, join("Resource '", key, "' has an empty category"));

    if (!is_non_negative(r.salvage_research_rp_per_ton)) {
      push(errors, join("Resource '", key, "' has invalid salvage_research_rp_per_ton: ",
                        r.salvage_research_rp_per_ton));
    }
  }

  // --- Components ---
  for (const auto& [key, c] : db.components) {
    if (key.empty()) push(errors, "Component map contains an empty key");
    if (c.id.empty()) push(errors, join("Component '", key, "' has an empty id field"));
    if (!c.id.empty() && !key.empty() && c.id != key)
      push(errors, join("Component key/id mismatch: key '", key, "' != id '", c.id, "'"));

    if (!is_non_negative(c.mass_tons))
      push(errors, join("Component '", key, "' has invalid mass_tons: ", c.mass_tons));
    if (!is_non_negative(c.speed_km_s))
      push(errors, join("Component '", key, "' has invalid speed_km_s: ", c.speed_km_s));
    if (!is_non_negative(c.fuel_use_per_mkm))
      push(errors, join("Component '", key, "' has invalid fuel_use_per_mkm: ", c.fuel_use_per_mkm));
    if (!is_non_negative(c.fuel_capacity_tons))
      push(errors, join("Component '", key, "' has invalid fuel_capacity_tons: ", c.fuel_capacity_tons));
    if (!is_non_negative(c.cargo_tons))
      push(errors, join("Component '", key, "' has invalid cargo_tons: ", c.cargo_tons));
    if (!is_non_negative(c.mining_tons_per_day))
      push(errors, join("Component '", key, "' has invalid mining_tons_per_day: ", c.mining_tons_per_day));
    if (!is_non_negative(c.sensor_range_mkm))
      push(errors, join("Component '", key, "' has invalid sensor_range_mkm: ", c.sensor_range_mkm));
    if (!is_non_negative(c.signature_multiplier))
      push(errors, join("Component '", key, "' has invalid signature_multiplier: ", c.signature_multiplier));
    else if (c.signature_multiplier > 1.0)
      push(errors, join("Component '", key, "' has signature_multiplier > 1.0 (expected [0,1]): ", c.signature_multiplier));
    if (!is_non_negative(c.colony_capacity_millions))
      push(errors,
           join("Component '", key, "' has invalid colony_capacity_millions: ", c.colony_capacity_millions));
    if (!is_non_negative(c.troop_capacity))
      push(errors, join("Component '", key, "' has invalid troop_capacity: ", c.troop_capacity));
    if (!is_non_negative(c.power_output))
      push(errors, join("Component '", key, "' has invalid power_output: ", c.power_output));
    if (!is_non_negative(c.power_use))
      push(errors, join("Component '", key, "' has invalid power_use: ", c.power_use));
    if (!is_non_negative(c.weapon_damage))
      push(errors, join("Component '", key, "' has invalid weapon_damage: ", c.weapon_damage));
    if (!is_non_negative(c.weapon_range_mkm))
      push(errors, join("Component '", key, "' has invalid weapon_range_mkm: ", c.weapon_range_mkm));
    if (!is_non_negative(c.missile_damage))
      push(errors, join("Component '", key, "' has invalid missile_damage: ", c.missile_damage));
    if (!is_non_negative(c.missile_range_mkm))
      push(errors, join("Component '", key, "' has invalid missile_range_mkm: ", c.missile_range_mkm));
    if (!is_non_negative(c.missile_speed_mkm_per_day))
      push(errors, join("Component '", key, "' has invalid missile_speed_mkm_per_day: ", c.missile_speed_mkm_per_day));
    if (!is_non_negative(c.missile_reload_days))
      push(errors, join("Component '", key, "' has invalid missile_reload_days: ", c.missile_reload_days));
    if (!is_non_negative(c.point_defense_damage))
      push(errors, join("Component '", key, "' has invalid point_defense_damage: ", c.point_defense_damage));
    if (!is_non_negative(c.point_defense_range_mkm))
      push(errors, join("Component '", key, "' has invalid point_defense_range_mkm: ", c.point_defense_range_mkm));
    if (!is_non_negative(c.hp_bonus))
      push(errors, join("Component '", key, "' has invalid hp_bonus: ", c.hp_bonus));
    if (!is_non_negative(c.shield_hp))
      push(errors, join("Component '", key, "' has invalid shield_hp: ", c.shield_hp));
    if (!is_non_negative(c.shield_regen_per_day))
      push(errors,
           join("Component '", key, "' has invalid shield_regen_per_day: ", c.shield_regen_per_day));
  }

  // --- Designs ---
  for (const auto& [key, d] : db.designs) {
    if (key.empty()) push(errors, "Design map contains an empty key");
    if (d.id.empty()) push(errors, join("Design '", key, "' has an empty id field"));
    if (!d.id.empty() && !key.empty() && d.id != key)
      push(errors, join("Design key/id mismatch: key '", key, "' != id '", d.id, "'"));

    if (d.components.empty()) {
      push(errors, join("Design '", key, "' has no components"));
    }

    for (const auto& cid : d.components) {
      if (db.components.find(cid) == db.components.end()) {
        push(errors, join("Design '", key, "' references unknown component id '", cid, "'"));
      }
    }

    if (!is_non_negative(d.mass_tons))
      push(errors, join("Design '", key, "' has invalid mass_tons: ", d.mass_tons));
    if (!is_non_negative(d.speed_km_s))
      push(errors, join("Design '", key, "' has invalid speed_km_s: ", d.speed_km_s));
    if (!is_non_negative(d.fuel_capacity_tons))
      push(errors, join("Design '", key, "' has invalid fuel_capacity_tons: ", d.fuel_capacity_tons));
    if (!is_non_negative(d.fuel_use_per_mkm))
      push(errors, join("Design '", key, "' has invalid fuel_use_per_mkm: ", d.fuel_use_per_mkm));
    if (d.fuel_use_per_mkm > 1e-9 && d.fuel_capacity_tons <= 1e-9)
      push(errors, join("Design '", key, "' consumes fuel but has zero fuel_capacity_tons"));
    if (!is_non_negative(d.cargo_tons))
      push(errors, join("Design '", key, "' has invalid cargo_tons: ", d.cargo_tons));
    if (!is_non_negative(d.mining_tons_per_day))
      push(errors, join("Design '", key, "' has invalid mining_tons_per_day: ", d.mining_tons_per_day));
    if (!is_non_negative(d.sensor_range_mkm))
      push(errors, join("Design '", key, "' has invalid sensor_range_mkm: ", d.sensor_range_mkm));
    if (!is_non_negative(d.signature_multiplier))
      push(errors, join("Design '", key, "' has invalid signature_multiplier: ", d.signature_multiplier));
    else if (d.signature_multiplier > 1.0)
      push(errors, join("Design '", key, "' has signature_multiplier > 1.0 (expected [0,1]): ", d.signature_multiplier));
    if (!is_non_negative(d.colony_capacity_millions))
      push(errors,
           join("Design '", key, "' has invalid colony_capacity_millions: ", d.colony_capacity_millions));

    if (!is_non_negative(d.power_generation))
      push(errors, join("Design '", key, "' has invalid power_generation: ", d.power_generation));
    if (!is_non_negative(d.power_use_total))
      push(errors, join("Design '", key, "' has invalid power_use_total: ", d.power_use_total));
    if (!is_non_negative(d.power_use_engines))
      push(errors, join("Design '", key, "' has invalid power_use_engines: ", d.power_use_engines));
    if (!is_non_negative(d.power_use_sensors))
      push(errors, join("Design '", key, "' has invalid power_use_sensors: ", d.power_use_sensors));
    if (!is_non_negative(d.power_use_weapons))
      push(errors, join("Design '", key, "' has invalid power_use_weapons: ", d.power_use_weapons));
    if (!is_non_negative(d.power_use_shields))
      push(errors, join("Design '", key, "' has invalid power_use_shields: ", d.power_use_shields));
    if (!is_non_negative(d.max_hp) || d.max_hp <= 0.0)
      push(errors, join("Design '", key, "' has invalid max_hp: ", d.max_hp));
    if (!is_non_negative(d.max_shields))
      push(errors, join("Design '", key, "' has invalid max_shields: ", d.max_shields));
    if (!is_non_negative(d.shield_regen_per_day))
      push(errors,
           join("Design '", key, "' has invalid shield_regen_per_day: ", d.shield_regen_per_day));
    if (!is_non_negative(d.weapon_damage))
      push(errors, join("Design '", key, "' has invalid weapon_damage: ", d.weapon_damage));
    if (!is_non_negative(d.weapon_range_mkm))
      push(errors, join("Design '", key, "' has invalid weapon_range_mkm: ", d.weapon_range_mkm));
    if (!is_non_negative(d.missile_damage))
      push(errors, join("Design '", key, "' has invalid missile_damage: ", d.missile_damage));
    if (!is_non_negative(d.missile_range_mkm))
      push(errors, join("Design '", key, "' has invalid missile_range_mkm: ", d.missile_range_mkm));
    if (!is_non_negative(d.missile_speed_mkm_per_day))
      push(errors,
           join("Design '", key, "' has invalid missile_speed_mkm_per_day: ", d.missile_speed_mkm_per_day));
    if (!is_non_negative(d.missile_reload_days))
      push(errors, join("Design '", key, "' has invalid missile_reload_days: ", d.missile_reload_days));
    if (!is_non_negative(d.point_defense_damage))
      push(errors, join("Design '", key, "' has invalid point_defense_damage: ", d.point_defense_damage));
    if (!is_non_negative(d.point_defense_range_mkm))
      push(errors, join("Design '", key, "' has invalid point_defense_range_mkm: ", d.point_defense_range_mkm));
  }

  // --- Installations ---
  for (const auto& [key, inst] : db.installations) {
    if (key.empty()) push(errors, "Installation map contains an empty key");
    if (inst.id.empty()) push(errors, join("Installation '", key, "' has an empty id field"));
    if (!inst.id.empty() && !key.empty() && inst.id != key)
      push(errors, join("Installation key/id mismatch: key '", key, "' != id '", inst.id, "'"));

    if (!is_non_negative(inst.construction_cost))
      push(errors, join("Installation '", key, "' has invalid construction_cost: ", inst.construction_cost));
    if (!is_non_negative(inst.construction_points_per_day))
      push(errors,
           join("Installation '", key, "' has invalid construction_points_per_day: ",
                inst.construction_points_per_day));
    if (!is_non_negative(inst.build_rate_tons_per_day))
      push(errors, join("Installation '", key, "' has invalid build_rate_tons_per_day: ", inst.build_rate_tons_per_day));
    if (!is_non_negative(inst.sensor_range_mkm))
      push(errors, join("Installation '", key, "' has invalid sensor_range_mkm: ", inst.sensor_range_mkm));
    if (!is_non_negative(inst.weapon_damage))
      push(errors, join("Installation '", key, "' has invalid weapon_damage: ", inst.weapon_damage));
    if (!is_non_negative(inst.weapon_range_mkm))
      push(errors, join("Installation '", key, "' has invalid weapon_range_mkm: ", inst.weapon_range_mkm));
    if (!is_non_negative(inst.research_points_per_day))
      push(errors, join("Installation '", key, "' has invalid research_points_per_day: ", inst.research_points_per_day));
    if (!is_non_negative(inst.terraforming_points_per_day))
      push(errors, join("Installation '", key, "' has invalid terraforming_points_per_day: ", inst.terraforming_points_per_day));
    if (!is_non_negative(inst.troop_training_points_per_day))
      push(errors, join("Installation '", key, "' has invalid troop_training_points_per_day: ", inst.troop_training_points_per_day));
    if (!is_non_negative(inst.habitation_capacity_millions))
      push(errors, join("Installation '", key, "' has invalid habitation_capacity_millions: ", inst.habitation_capacity_millions));
    if (!is_non_negative(inst.fortification_points))
      push(errors, join("Installation '", key, "' has invalid fortification_points: ", inst.fortification_points));

    if (!is_non_negative(inst.mining_tons_per_day))
      push(errors, join("Installation '", key, "' has invalid mining_tons_per_day: ", inst.mining_tons_per_day));

    for (const auto& [mineral, amount] : inst.produces_per_day) {
      if (mineral.empty()) push(errors, join("Installation '", key, "' produces an empty mineral id"));
      if (!resource_known(mineral))
        push(errors, join("Installation '", key, "' references unknown resource '", mineral, "'"));
      if (!is_non_negative(amount))
        push(errors, join("Installation '", key, "' has invalid production for '", mineral, "': ", amount));
    }
    for (const auto& [mineral, amount] : inst.consumes_per_day) {
      if (mineral.empty()) push(errors, join("Installation '", key, "' consumes an empty mineral id"));
      if (!resource_known(mineral))
        push(errors, join("Installation '", key, "' references unknown resource '", mineral, "'"));
      if (!is_non_negative(amount))
        push(errors, join("Installation '", key, "' has invalid consumption for '", mineral, "': ", amount));
    }
    for (const auto& [mineral, amount] : inst.build_costs) {
      if (mineral.empty()) push(errors, join("Installation '", key, "' has a build_cost with empty mineral id"));
      if (!resource_known(mineral))
        push(errors, join("Installation '", key, "' references unknown resource '", mineral, "'"));
      if (!is_non_negative(amount))
        push(errors, join("Installation '", key, "' has invalid build_cost for '", mineral, "': ", amount));
    }
    for (const auto& [mineral, amount] : inst.build_costs_per_ton) {
      if (mineral.empty()) push(errors, join("Installation '", key, "' has a build_costs_per_ton with empty mineral id"));
      if (!resource_known(mineral))
        push(errors, join("Installation '", key, "' references unknown resource '", mineral, "'"));
      if (!is_non_negative(amount))
        push(errors, join("Installation '", key, "' has invalid build_costs_per_ton for '", mineral, "': ", amount));
    }
  }

  // --- Tech tree ---
  for (const auto& [key, t] : db.techs) {
    if (key.empty()) push(errors, "Tech map contains an empty key");
    if (t.id.empty()) push(errors, join("Tech '", key, "' has an empty id field"));
    if (!t.id.empty() && !key.empty() && t.id != key)
      push(errors, join("Tech key/id mismatch: key '", key, "' != id '", t.id, "'"));
    if (t.name.empty()) push(errors, join("Tech '", key, "' has an empty name field"));
    if (!is_non_negative(t.cost)) push(errors, join("Tech '", key, "' has invalid cost: ", t.cost));

    for (const auto& prereq : t.prereqs) {
      if (prereq.empty()) {
        push(errors, join("Tech '", key, "' has an empty prereq tech id"));
        continue;
      }
      if (prereq == key) {
        push(errors, join("Tech '", key, "' lists itself as a prerequisite"));
        continue;
      }
      if (db.techs.find(prereq) == db.techs.end()) {
        push(errors, join("Tech '", key, "' references unknown prereq tech '", prereq, "'"));
      }
    }

    for (const auto& eff : t.effects) {
      if (eff.type.empty()) {
        push(errors, join("Tech '", key, "' has an effect with empty type"));
        continue;
      }
      if (eff.value.empty()) {
        push(errors, join("Tech '", key, "' has an effect with empty value"));
        continue;
      }

      if (eff.type == "unlock_component") {
        if (db.components.find(eff.value) == db.components.end()) {
          push(errors, join("Tech '", key, "' unlocks unknown component '", eff.value, "'"));
        }
      } else if (eff.type == "unlock_installation") {
        if (db.installations.find(eff.value) == db.installations.end()) {
          push(errors, join("Tech '", key, "' unlocks unknown installation '", eff.value, "'"));
        }
      } else {
        // Unknown effect types are effectively ignored by the simulation.
        push(errors, join("Tech '", key, "' has unknown effect type '", eff.type, "'"));
      }
    }
  }

  // Detect prereq cycles (these can deadlock research).
  {
    std::vector<std::string> tech_ids;
    tech_ids.reserve(db.techs.size());
    for (const auto& [id, _] : db.techs) tech_ids.push_back(id);
    std::sort(tech_ids.begin(), tech_ids.end());

    // 0 = unvisited, 1 = visiting, 2 = done.
    std::unordered_map<std::string, int> visit;
    visit.reserve(db.techs.size() * 2);

    std::vector<std::string> stack;
    stack.reserve(db.techs.size());

    std::unordered_map<std::string, std::size_t> stack_pos;
    stack_pos.reserve(db.techs.size() * 2);

    // Track cycles we've already reported (canonicalized as sorted ids joined by '|').
    std::unordered_set<std::string> reported;
    reported.reserve(db.techs.size() * 2);

    std::function<void(const std::string&)> dfs = [&](const std::string& id) {
      visit[id] = 1;
      stack_pos[id] = stack.size();
      stack.push_back(id);

      const auto it = db.techs.find(id);
      if (it != db.techs.end()) {
        // Deterministic: iterate prereqs in sorted/unique order.
        std::vector<std::string> prereqs = it->second.prereqs;
        std::sort(prereqs.begin(), prereqs.end());
        prereqs.erase(std::unique(prereqs.begin(), prereqs.end()), prereqs.end());

        for (const auto& pre : prereqs) {
          if (pre.empty()) continue;
          if (db.techs.find(pre) == db.techs.end()) continue;

          const auto vit = visit.find(pre);
          const int st = (vit == visit.end()) ? 0 : vit->second;

          if (st == 0) {
            dfs(pre);
          } else if (st == 1) {
            // Cycle detected. Extract the cycle portion of the stack.
            std::size_t idx = 0;
            if (const auto sit = stack_pos.find(pre); sit != stack_pos.end()) idx = sit->second;

            std::vector<std::string> cycle(stack.begin() + static_cast<std::ptrdiff_t>(idx), stack.end());
            std::sort(cycle.begin(), cycle.end());
            cycle.erase(std::unique(cycle.begin(), cycle.end()), cycle.end());

            std::ostringstream key_ss;
            for (std::size_t i = 0; i < cycle.size(); ++i) {
              if (i) key_ss << "|";
              key_ss << cycle[i];
            }
            const std::string key = key_ss.str();

            if (reported.insert(key).second) {
              std::ostringstream ss;
              for (std::size_t i = 0; i < cycle.size(); ++i) {
                if (i) ss << ", ";
                ss << "'" << cycle[i] << "'";
              }
              push(errors, "Tech prerequisite cycle detected among: " + ss.str());
            }
          }
        }
      }

      stack.pop_back();
      stack_pos.erase(id);
      visit[id] = 2;
    };

    for (const auto& id : tech_ids) {
      const auto vit = visit.find(id);
      const int st = (vit == visit.end()) ? 0 : vit->second;
      if (st == 0) dfs(id);
    }
  }

  // Sort so output is stable for tests/CI.
  std::sort(errors.begin(), errors.end());
  return errors;
}

} // namespace nebula4x
