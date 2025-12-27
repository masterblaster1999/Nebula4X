#include "nebula4x/core/content_validation.h"

#include <algorithm>
#include <cmath>
#include <sstream>
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
    if (!is_non_negative(c.cargo_tons))
      push(errors, join("Component '", key, "' has invalid cargo_tons: ", c.cargo_tons));
    if (!is_non_negative(c.sensor_range_mkm))
      push(errors, join("Component '", key, "' has invalid sensor_range_mkm: ", c.sensor_range_mkm));
    if (!is_non_negative(c.power))
      push(errors, join("Component '", key, "' has invalid power: ", c.power));
    if (!is_non_negative(c.weapon_damage))
      push(errors, join("Component '", key, "' has invalid weapon_damage: ", c.weapon_damage));
    if (!is_non_negative(c.weapon_range_mkm))
      push(errors, join("Component '", key, "' has invalid weapon_range_mkm: ", c.weapon_range_mkm));
    if (!is_non_negative(c.hp_bonus))
      push(errors, join("Component '", key, "' has invalid hp_bonus: ", c.hp_bonus));
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
    if (!is_non_negative(d.cargo_tons))
      push(errors, join("Design '", key, "' has invalid cargo_tons: ", d.cargo_tons));
    if (!is_non_negative(d.sensor_range_mkm))
      push(errors, join("Design '", key, "' has invalid sensor_range_mkm: ", d.sensor_range_mkm));
    if (!is_non_negative(d.max_hp) || d.max_hp <= 0.0)
      push(errors, join("Design '", key, "' has invalid max_hp: ", d.max_hp));
    if (!is_non_negative(d.weapon_damage))
      push(errors, join("Design '", key, "' has invalid weapon_damage: ", d.weapon_damage));
    if (!is_non_negative(d.weapon_range_mkm))
      push(errors, join("Design '", key, "' has invalid weapon_range_mkm: ", d.weapon_range_mkm));
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
    if (!is_non_negative(inst.research_points_per_day))
      push(errors, join("Installation '", key, "' has invalid research_points_per_day: ", inst.research_points_per_day));

    for (const auto& [mineral, amount] : inst.produces_per_day) {
      if (mineral.empty()) push(errors, join("Installation '", key, "' produces an empty mineral id"));
      if (!is_non_negative(amount))
        push(errors, join("Installation '", key, "' has invalid production for '", mineral, "': ", amount));
    }
    for (const auto& [mineral, amount] : inst.build_costs) {
      if (mineral.empty()) push(errors, join("Installation '", key, "' has a build_cost with empty mineral id"));
      if (!is_non_negative(amount))
        push(errors, join("Installation '", key, "' has invalid build_cost for '", mineral, "': ", amount));
    }
    for (const auto& [mineral, amount] : inst.build_costs_per_ton) {
      if (mineral.empty()) push(errors, join("Installation '", key, "' has a build_costs_per_ton with empty mineral id"));
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
    if (!is_non_negative(t.cost)) push(errors, join("Tech '", key, "' has invalid cost: ", t.cost));

    for (const auto& prereq : t.prereqs) {
      if (db.techs.find(prereq) == db.techs.end()) {
        push(errors, join("Tech '", key, "' references unknown prereq tech '", prereq, "'"));
      }
    }

    for (const auto& eff : t.effects) {
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

  // Sort so output is stable for tests/CI.
  std::sort(errors.begin(), errors.end());
  return errors;
}

} // namespace nebula4x
