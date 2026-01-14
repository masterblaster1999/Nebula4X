#include "nebula4x/core/content_validation.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <functional>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace nebula4x {

namespace {

void push_issue(std::vector<ContentIssue>& out, ContentIssueSeverity sev, std::string code, std::string msg,
                std::string subject_kind = {}, std::string subject_id = {}) {
  ContentIssue is;
  is.severity = sev;
  is.code = std::move(code);
  is.message = std::move(msg);
  is.subject_kind = std::move(subject_kind);
  is.subject_id = std::move(subject_id);
  out.push_back(std::move(is));
}

void push_error(std::vector<ContentIssue>& out, std::string code, std::string msg, std::string subject_kind = {},
                std::string subject_id = {}) {
  push_issue(out, ContentIssueSeverity::Error, std::move(code), std::move(msg), std::move(subject_kind),
             std::move(subject_id));
}

void push_warning(std::vector<ContentIssue>& out, std::string code, std::string msg, std::string subject_kind = {},
                  std::string subject_id = {}) {
  push_issue(out, ContentIssueSeverity::Warning, std::move(code), std::move(msg), std::move(subject_kind),
             std::move(subject_id));
}

template <typename... Parts>
std::string join(Parts&&... parts) {
  std::ostringstream ss;
  (ss << ... << std::forward<Parts>(parts));
  return ss.str();
}

std::string ascii_to_lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

bool is_finite(double v) { return std::isfinite(v); }

bool is_known_faction_output_key(const std::string& k) {
  // Keep in sync with sim_internal::apply_factor() (simulation_internal.h).
  return k == "all" || k == "mining" || k == "industry" || k == "research" || k == "construction" ||
         k == "construction_points" || k == "construction_point" || k == "shipyard" || k == "terraforming" ||
         k == "troop_training" || k == "training";
}

bool is_non_negative(double v) { return v >= 0.0 && std::isfinite(v); }

} // namespace

std::vector<ContentIssue> validate_content_db_detailed(const ContentDB& db) {
  std::vector<ContentIssue> issues;

  const auto resource_known = [&](const std::string& rid) -> bool {
    if (db.resources.empty()) return true;
    return db.resources.find(rid) != db.resources.end();
  };

  // --- Resources (optional) ---
  for (const auto& [key, r] : db.resources) {
    if (key.empty()) push_error(issues, "resource.empty_key", "Resource map contains an empty key", "resource", key);
    if (r.id.empty())
      push_error(issues, "resource.empty_id", join("Resource '", key, "' has an empty id field"), "resource", key);
    if (!r.id.empty() && !key.empty() && r.id != key)
      push_error(issues, "resource.key_id_mismatch",
                 join("Resource key/id mismatch: key '", key, "' != id '", r.id, "'"), "resource", key);
    if (r.name.empty())
      push_error(issues, "resource.empty_name", join("Resource '", key, "' has an empty name"), "resource", key);
    if (r.category.empty())
      push_error(issues, "resource.empty_category", join("Resource '", key, "' has an empty category"), "resource", key);

    if (!is_non_negative(r.salvage_research_rp_per_ton)) {
      push_error(issues, "resource.invalid_salvage_rp",
                 join("Resource '", key, "' has invalid salvage_research_rp_per_ton: ", r.salvage_research_rp_per_ton),
                 "resource", key);
    }
  }

  // --- Components ---
  for (const auto& [key, c] : db.components) {
    if (key.empty()) push_error(issues, "component.empty_key", "Component map contains an empty key", "component", key);
    if (c.id.empty())
      push_error(issues, "component.empty_id", join("Component '", key, "' has an empty id field"), "component", key);
    if (!c.id.empty() && !key.empty() && c.id != key)
      push_error(issues, "component.key_id_mismatch",
                 join("Component key/id mismatch: key '", key, "' != id '", c.id, "'"), "component", key);

    if (!is_non_negative(c.mass_tons))
      push_error(issues, "component.invalid_mass", join("Component '", key, "' has invalid mass_tons: ", c.mass_tons),
                 "component", key);
    if (!is_non_negative(c.speed_km_s))
      push_error(issues, "component.invalid_speed",
                 join("Component '", key, "' has invalid speed_km_s: ", c.speed_km_s), "component", key);
    if (!is_non_negative(c.fuel_use_per_mkm))
      push_error(issues, "component.invalid_fuel_use",
                 join("Component '", key, "' has invalid fuel_use_per_mkm: ", c.fuel_use_per_mkm), "component", key);
    if (!is_non_negative(c.fuel_capacity_tons))
      push_error(issues, "component.invalid_fuel_cap",
                 join("Component '", key, "' has invalid fuel_capacity_tons: ", c.fuel_capacity_tons), "component", key);
    if (!is_non_negative(c.cargo_tons))
      push_error(issues, "component.invalid_cargo", join("Component '", key, "' has invalid cargo_tons: ", c.cargo_tons),
                 "component", key);
    if (!is_non_negative(c.mining_tons_per_day))
      push_error(issues, "component.invalid_mining",
                 join("Component '", key, "' has invalid mining_tons_per_day: ", c.mining_tons_per_day), "component",
                 key);
    if (!is_non_negative(c.sensor_range_mkm))
      push_error(issues, "component.invalid_sensor",
                 join("Component '", key, "' has invalid sensor_range_mkm: ", c.sensor_range_mkm), "component", key);
    if (!is_non_negative(c.signature_multiplier))
      push_error(issues, "component.invalid_signature",
                 join("Component '", key, "' has invalid signature_multiplier: ", c.signature_multiplier), "component",
                 key);
    else if (c.signature_multiplier > 1.0)
      push_error(issues, "component.signature_gt1",
                 join("Component '", key,
                      "' has signature_multiplier > 1.0 (expected [0,1]): ", c.signature_multiplier),
                 "component", key);
if (!is_non_negative(c.ecm_strength))
  push_error(issues, "component.invalid_ecm",
             join("Component '", key, "' has invalid ecm_strength: ", c.ecm_strength), "component", key);
if (!is_non_negative(c.eccm_strength))
  push_error(issues, "component.invalid_eccm",
             join("Component '", key, "' has invalid eccm_strength: ", c.eccm_strength), "component", key);
    if (!is_non_negative(c.colony_capacity_millions))
      push_error(issues, "component.invalid_colony_cap",
                 join("Component '", key, "' has invalid colony_capacity_millions: ", c.colony_capacity_millions),
                 "component", key);
    if (!is_non_negative(c.troop_capacity))
      push_error(issues, "component.invalid_troop_cap",
                 join("Component '", key, "' has invalid troop_capacity: ", c.troop_capacity), "component", key);
    if (!is_non_negative(c.power_output))
      push_error(issues, "component.invalid_power_out",
                 join("Component '", key, "' has invalid power_output: ", c.power_output), "component", key);
    if (!is_non_negative(c.power_use))
      push_error(issues, "component.invalid_power_use", join("Component '", key, "' has invalid power_use: ", c.power_use),
                 "component", key);
    if (!is_non_negative(c.weapon_damage))
      push_error(issues, "component.invalid_weapon_damage",
                 join("Component '", key, "' has invalid weapon_damage: ", c.weapon_damage), "component", key);
    if (!is_non_negative(c.weapon_range_mkm))
      push_error(issues, "component.invalid_weapon_range",
                 join("Component '", key, "' has invalid weapon_range_mkm: ", c.weapon_range_mkm), "component", key);
    if (!is_non_negative(c.missile_damage))
      push_error(issues, "component.invalid_missile_damage",
                 join("Component '", key, "' has invalid missile_damage: ", c.missile_damage), "component", key);
    if (!is_non_negative(c.missile_range_mkm))
      push_error(issues, "component.invalid_missile_range",
                 join("Component '", key, "' has invalid missile_range_mkm: ", c.missile_range_mkm), "component", key);
    if (!is_non_negative(c.missile_speed_mkm_per_day))
      push_error(issues, "component.invalid_missile_speed",
                 join("Component '", key, "' has invalid missile_speed_mkm_per_day: ", c.missile_speed_mkm_per_day),
                 "component", key);
    if (!is_non_negative(c.missile_reload_days))
      push_error(issues, "component.invalid_missile_reload",
                 join("Component '", key, "' has invalid missile_reload_days: ", c.missile_reload_days), "component",
                 key);
    if (c.missile_ammo < 0)
      push_error(issues, "component.invalid_missile_ammo",
                 join("Component '", key, "' has invalid missile_ammo: ", c.missile_ammo), "component", key);
    if (!is_non_negative(c.point_defense_damage))
      push_error(issues, "component.invalid_pd_damage",
                 join("Component '", key, "' has invalid point_defense_damage: ", c.point_defense_damage), "component",
                 key);
    if (!is_non_negative(c.point_defense_range_mkm))
      push_error(issues, "component.invalid_pd_range",
                 join("Component '", key, "' has invalid point_defense_range_mkm: ", c.point_defense_range_mkm),
                 "component", key);
    if (!is_non_negative(c.hp_bonus))
      push_error(issues, "component.invalid_hp_bonus", join("Component '", key, "' has invalid hp_bonus: ", c.hp_bonus),
                 "component", key);
    if (!is_non_negative(c.shield_hp))
      push_error(issues, "component.invalid_shield_hp",
                 join("Component '", key, "' has invalid shield_hp: ", c.shield_hp), "component", key);
    if (!is_non_negative(c.shield_regen_per_day))
      push_error(issues, "component.invalid_shield_regen",
                 join("Component '", key, "' has invalid shield_regen_per_day: ", c.shield_regen_per_day), "component",
                 key);
    if (!is_non_negative(c.heat_generation_per_day))
      push_error(issues, "component.invalid_heat_gen",
                 join("Component '", key, "' has invalid heat_generation_per_day: ", c.heat_generation_per_day),
                 "component", key);
    if (!is_non_negative(c.heat_dissipation_per_day))
      push_error(issues, "component.invalid_heat_diss",
                 join("Component '", key, "' has invalid heat_dissipation_per_day: ", c.heat_dissipation_per_day),
                 "component", key);
    if (!is_non_negative(c.heat_capacity))
      push_error(issues, "component.invalid_heat_cap",
                 join("Component '", key, "' has invalid heat_capacity: ", c.heat_capacity), "component", key);
  }

  // --- Designs ---
  for (const auto& [key, d] : db.designs) {
    if (key.empty()) push_error(issues, "design.empty_key", "Design map contains an empty key", "design", key);
    if (d.id.empty())
      push_error(issues, "design.empty_id", join("Design '", key, "' has an empty id field"), "design", key);
    if (!d.id.empty() && !key.empty() && d.id != key)
      push_error(issues, "design.key_id_mismatch",
                 join("Design key/id mismatch: key '", key, "' != id '", d.id, "'"), "design", key);

    if (d.components.empty()) {
      push_error(issues, "design.empty_components", join("Design '", key, "' has no components"), "design", key);
    }

    for (const auto& cid : d.components) {
      if (db.components.find(cid) == db.components.end()) {
        push_error(issues, "design.unknown_component",
                   join("Design '", key, "' references unknown component id '", cid, "'"), "design", key);
      }
    }

    if (!is_non_negative(d.mass_tons))
      push_error(issues, "design.invalid_mass", join("Design '", key, "' has invalid mass_tons: ", d.mass_tons), "design",
                 key);
    if (!is_non_negative(d.speed_km_s))
      push_error(issues, "design.invalid_speed", join("Design '", key, "' has invalid speed_km_s: ", d.speed_km_s),
                 "design", key);
    if (!is_non_negative(d.fuel_capacity_tons))
      push_error(issues, "design.invalid_fuel_cap",
                 join("Design '", key, "' has invalid fuel_capacity_tons: ", d.fuel_capacity_tons), "design", key);
    if (!is_non_negative(d.fuel_use_per_mkm))
      push_error(issues, "design.invalid_fuel_use",
                 join("Design '", key, "' has invalid fuel_use_per_mkm: ", d.fuel_use_per_mkm), "design", key);
    if (d.fuel_use_per_mkm > 1e-9 && d.fuel_capacity_tons <= 1e-9)
      push_error(issues, "design.fuel_use_zero_cap",
                 join("Design '", key, "' consumes fuel but has zero fuel_capacity_tons"), "design", key);
    if (!is_non_negative(d.cargo_tons))
      push_error(issues, "design.invalid_cargo", join("Design '", key, "' has invalid cargo_tons: ", d.cargo_tons),
                 "design", key);
    if (!is_non_negative(d.mining_tons_per_day))
      push_error(issues, "design.invalid_mining", join("Design '", key, "' has invalid mining_tons_per_day: ",
                                                      d.mining_tons_per_day),
                 "design", key);
    if (!is_non_negative(d.sensor_range_mkm))
      push_error(issues, "design.invalid_sensor",
                 join("Design '", key, "' has invalid sensor_range_mkm: ", d.sensor_range_mkm), "design", key);
    if (!is_non_negative(d.signature_multiplier))
      push_error(issues, "design.invalid_signature",
                 join("Design '", key, "' has invalid signature_multiplier: ", d.signature_multiplier), "design", key);
    else if (d.signature_multiplier > 1.0)
      push_error(issues, "design.signature_gt1",
                 join("Design '", key, "' has signature_multiplier > 1.0 (expected [0,1]): ", d.signature_multiplier),
                 "design", key);
if (!is_non_negative(d.ecm_strength))
  push_error(issues, "design.invalid_ecm",
             join("Design '", key, "' has invalid ecm_strength: ", d.ecm_strength), "design", key);
if (!is_non_negative(d.eccm_strength))
  push_error(issues, "design.invalid_eccm",
             join("Design '", key, "' has invalid eccm_strength: ", d.eccm_strength), "design", key);
    if (!is_non_negative(d.colony_capacity_millions))
      push_error(issues, "design.invalid_colony_cap",
                 join("Design '", key, "' has invalid colony_capacity_millions: ", d.colony_capacity_millions), "design",
                 key);

    if (!is_non_negative(d.power_generation))
      push_error(issues, "design.invalid_power_gen",
                 join("Design '", key, "' has invalid power_generation: ", d.power_generation), "design", key);
    if (!is_non_negative(d.power_use_total))
      push_error(issues, "design.invalid_power_total",
                 join("Design '", key, "' has invalid power_use_total: ", d.power_use_total), "design", key);
    if (!is_non_negative(d.power_use_engines))
      push_error(issues, "design.invalid_power_engines",
                 join("Design '", key, "' has invalid power_use_engines: ", d.power_use_engines), "design", key);
    if (!is_non_negative(d.power_use_sensors))
      push_error(issues, "design.invalid_power_sensors",
                 join("Design '", key, "' has invalid power_use_sensors: ", d.power_use_sensors), "design", key);
    if (!is_non_negative(d.power_use_weapons))
      push_error(issues, "design.invalid_power_weapons",
                 join("Design '", key, "' has invalid power_use_weapons: ", d.power_use_weapons), "design", key);
    if (!is_non_negative(d.power_use_shields))
      push_error(issues, "design.invalid_power_shields",
                 join("Design '", key, "' has invalid power_use_shields: ", d.power_use_shields), "design", key);
    if (!is_non_negative(d.max_hp) || d.max_hp <= 0.0)
      push_error(issues, "design.invalid_max_hp", join("Design '", key, "' has invalid max_hp: ", d.max_hp), "design",
                 key);
    if (!is_non_negative(d.max_shields))
      push_error(issues, "design.invalid_max_shields", join("Design '", key, "' has invalid max_shields: ", d.max_shields),
                 "design", key);
    if (!is_non_negative(d.shield_regen_per_day))
      push_error(issues, "design.invalid_shield_regen",
                 join("Design '", key, "' has invalid shield_regen_per_day: ", d.shield_regen_per_day), "design", key);
    if (!is_non_negative(d.heat_capacity_bonus))
      push_error(issues, "design.invalid_heat_cap",
                 join("Design '", key, "' has invalid heat_capacity_bonus: ", d.heat_capacity_bonus), "design", key);
    if (!is_non_negative(d.heat_generation_bonus_per_day))
      push_error(issues, "design.invalid_heat_gen",
                 join("Design '", key, "' has invalid heat_generation_bonus_per_day: ", d.heat_generation_bonus_per_day),
                 "design", key);
    if (!is_non_negative(d.heat_dissipation_bonus_per_day))
      push_error(issues, "design.invalid_heat_diss",
                 join("Design '", key, "' has invalid heat_dissipation_bonus_per_day: ", d.heat_dissipation_bonus_per_day),
                 "design", key);
    if (!is_non_negative(d.weapon_damage))
      push_error(issues, "design.invalid_weapon_damage",
                 join("Design '", key, "' has invalid weapon_damage: ", d.weapon_damage), "design", key);
    if (!is_non_negative(d.weapon_range_mkm))
      push_error(issues, "design.invalid_weapon_range",
                 join("Design '", key, "' has invalid weapon_range_mkm: ", d.weapon_range_mkm), "design", key);
    if (!is_non_negative(d.missile_damage))
      push_error(issues, "design.invalid_missile_damage",
                 join("Design '", key, "' has invalid missile_damage: ", d.missile_damage), "design", key);
    if (!is_non_negative(d.missile_range_mkm))
      push_error(issues, "design.invalid_missile_range",
                 join("Design '", key, "' has invalid missile_range_mkm: ", d.missile_range_mkm), "design", key);
    if (!is_non_negative(d.missile_speed_mkm_per_day))
      push_error(issues, "design.invalid_missile_speed",
                 join("Design '", key, "' has invalid missile_speed_mkm_per_day: ", d.missile_speed_mkm_per_day),
                 "design", key);
    if (!is_non_negative(d.missile_reload_days))
      push_error(issues, "design.invalid_missile_reload",
                 join("Design '", key, "' has invalid missile_reload_days: ", d.missile_reload_days), "design", key);
    if (d.missile_launcher_count < 0)
      push_error(issues, "design.invalid_missile_launcher_count",
                 join("Design '", key, "' has invalid missile_launcher_count: ", d.missile_launcher_count), "design",
                 key);
    if (d.missile_ammo_capacity < 0)
      push_error(issues, "design.invalid_missile_ammo_capacity",
                 join("Design '", key, "' has invalid missile_ammo_capacity: ", d.missile_ammo_capacity), "design",
                 key);
    if (!is_non_negative(d.point_defense_damage))
      push_error(issues, "design.invalid_pd_damage",
                 join("Design '", key, "' has invalid point_defense_damage: ", d.point_defense_damage), "design", key);
    if (!is_non_negative(d.point_defense_range_mkm))
      push_error(issues, "design.invalid_pd_range",
                 join("Design '", key, "' has invalid point_defense_range_mkm: ", d.point_defense_range_mkm), "design",
                 key);
  }

  // --- Installations ---
  for (const auto& [key, inst] : db.installations) {
    if (key.empty())
      push_error(issues, "installation.empty_key", "Installation map contains an empty key", "installation", key);
    if (inst.id.empty())
      push_error(issues, "installation.empty_id", join("Installation '", key, "' has an empty id field"), "installation",
                 key);
    if (!inst.id.empty() && !key.empty() && inst.id != key)
      push_error(issues, "installation.key_id_mismatch",
                 join("Installation key/id mismatch: key '", key, "' != id '", inst.id, "'"), "installation", key);

    if (!is_non_negative(inst.construction_cost))
      push_error(issues, "installation.invalid_construction_cost",
                 join("Installation '", key, "' has invalid construction_cost: ", inst.construction_cost), "installation",
                 key);
    if (!is_non_negative(inst.construction_points_per_day))
      push_error(issues, "installation.invalid_construction_ppd",
                 join("Installation '", key,
                      "' has invalid construction_points_per_day: ", inst.construction_points_per_day),
                 "installation", key);
    if (!is_non_negative(inst.build_rate_tons_per_day))
      push_error(issues, "installation.invalid_build_rate",
                 join("Installation '", key, "' has invalid build_rate_tons_per_day: ", inst.build_rate_tons_per_day),
                 "installation", key);
    if (!is_non_negative(inst.sensor_range_mkm))
      push_error(issues, "installation.invalid_sensor",
                 join("Installation '", key, "' has invalid sensor_range_mkm: ", inst.sensor_range_mkm), "installation",
                 key);
    if (!is_non_negative(inst.weapon_damage))
      push_error(issues, "installation.invalid_weapon_damage",
                 join("Installation '", key, "' has invalid weapon_damage: ", inst.weapon_damage), "installation", key);
    if (!is_non_negative(inst.weapon_range_mkm))
      push_error(issues, "installation.invalid_weapon_range",
                 join("Installation '", key, "' has invalid weapon_range_mkm: ", inst.weapon_range_mkm), "installation",
                 key);

    if (!is_non_negative(inst.point_defense_damage))
      push_error(issues, "installation.invalid_pd_damage",
                 join("Installation '", key, "' has invalid point_defense_damage: ", inst.point_defense_damage),
                 "installation", key);
    if (!is_non_negative(inst.point_defense_range_mkm))
      push_error(issues, "installation.invalid_pd_range",
                 join("Installation '", key, "' has invalid point_defense_range_mkm: ", inst.point_defense_range_mkm),
                 "installation", key);
    if (!is_non_negative(inst.research_points_per_day))
      push_error(issues, "installation.invalid_rp",
                 join("Installation '", key, "' has invalid research_points_per_day: ", inst.research_points_per_day),
                 "installation", key);
    if (!is_non_negative(inst.terraforming_points_per_day))
      push_error(issues, "installation.invalid_terraform",
                 join("Installation '", key,
                      "' has invalid terraforming_points_per_day: ", inst.terraforming_points_per_day),
                 "installation", key);
    if (!is_non_negative(inst.troop_training_points_per_day))
      push_error(issues, "installation.invalid_training",
                 join("Installation '", key,
                      "' has invalid troop_training_points_per_day: ", inst.troop_training_points_per_day),
                 "installation", key);
    if (!is_non_negative(inst.crew_training_points_per_day))
      push_error(issues, "installation.invalid_crew_training",
                 join("Installation '", key,
                      "' has invalid crew_training_points_per_day: ", inst.crew_training_points_per_day),
                 "installation", key);
    if (!is_non_negative(inst.habitation_capacity_millions))
      push_error(issues, "installation.invalid_hab_cap",
                 join("Installation '", key,
                      "' has invalid habitation_capacity_millions: ", inst.habitation_capacity_millions),
                 "installation", key);
    if (!is_non_negative(inst.fortification_points))
      push_error(issues, "installation.invalid_fort",
                 join("Installation '", key, "' has invalid fortification_points: ", inst.fortification_points),
                 "installation", key);

    if (!is_non_negative(inst.mining_tons_per_day))
      push_error(issues, "installation.invalid_mining",
                 join("Installation '", key, "' has invalid mining_tons_per_day: ", inst.mining_tons_per_day),
                 "installation", key);

    for (const auto& [mineral, amount] : inst.produces_per_day) {
      if (mineral.empty())
        push_error(issues, "installation.produces_empty_id",
                   join("Installation '", key, "' produces an empty mineral id"), "installation", key);
      if (!resource_known(mineral))
        push_error(issues, "installation.unknown_resource",
                   join("Installation '", key, "' references unknown resource '", mineral, "'"), "installation", key);
      if (!is_non_negative(amount))
        push_error(issues, "installation.invalid_production",
                   join("Installation '", key, "' has invalid production for '", mineral, "': ", amount), "installation",
                   key);
    }
    for (const auto& [mineral, amount] : inst.consumes_per_day) {
      if (mineral.empty())
        push_error(issues, "installation.consumes_empty_id",
                   join("Installation '", key, "' consumes an empty mineral id"), "installation", key);
      if (!resource_known(mineral))
        push_error(issues, "installation.unknown_resource",
                   join("Installation '", key, "' references unknown resource '", mineral, "'"), "installation", key);
      if (!is_non_negative(amount))
        push_error(issues, "installation.invalid_consumption",
                   join("Installation '", key, "' has invalid consumption for '", mineral, "': ", amount),
                   "installation", key);
    }
    for (const auto& [mineral, amount] : inst.build_costs) {
      if (mineral.empty())
        push_error(issues, "installation.build_cost_empty_id",
                   join("Installation '", key, "' has a build_cost with empty mineral id"), "installation", key);
      if (!resource_known(mineral))
        push_error(issues, "installation.unknown_resource",
                   join("Installation '", key, "' references unknown resource '", mineral, "'"), "installation", key);
      if (!is_non_negative(amount))
        push_error(issues, "installation.invalid_build_cost",
                   join("Installation '", key, "' has invalid build_cost for '", mineral, "': ", amount), "installation",
                   key);
    }
    for (const auto& [mineral, amount] : inst.build_costs_per_ton) {
      if (mineral.empty())
        push_error(issues, "installation.build_cost_per_ton_empty_id",
                   join("Installation '", key, "' has a build_costs_per_ton with empty mineral id"), "installation",
                   key);
      if (!resource_known(mineral))
        push_error(issues, "installation.unknown_resource",
                   join("Installation '", key, "' references unknown resource '", mineral, "'"), "installation", key);
      if (!is_non_negative(amount))
        push_error(issues, "installation.invalid_build_cost_per_ton",
                   join("Installation '", key, "' has invalid build_costs_per_ton for '", mineral, "': ", amount),
                   "installation", key);
    }
  }

  // --- Tech tree ---
  for (const auto& [key, t] : db.techs) {
    if (key.empty()) push_error(issues, "tech.empty_key", "Tech map contains an empty key", "tech", key);
    if (t.id.empty()) push_error(issues, "tech.empty_id", join("Tech '", key, "' has an empty id field"), "tech", key);
    if (!t.id.empty() && !key.empty() && t.id != key)
      push_error(issues, "tech.key_id_mismatch", join("Tech key/id mismatch: key '", key, "' != id '", t.id, "'"),
                 "tech", key);
    if (t.name.empty())
      push_error(issues, "tech.empty_name", join("Tech '", key, "' has an empty name field"), "tech", key);
    if (!is_non_negative(t.cost))
      push_error(issues, "tech.invalid_cost", join("Tech '", key, "' has invalid cost: ", t.cost), "tech", key);

    for (const auto& prereq : t.prereqs) {
      if (prereq.empty()) {
        push_error(issues, "tech.empty_prereq", join("Tech '", key, "' has an empty prereq tech id"), "tech", key);
        continue;
      }
      if (prereq == key) {
        push_error(issues, "tech.self_prereq", join("Tech '", key, "' lists itself as a prerequisite"), "tech", key);
        continue;
      }
      if (db.techs.find(prereq) == db.techs.end()) {
        push_error(issues, "tech.unknown_prereq",
                   join("Tech '", key, "' references unknown prereq tech '", prereq, "'"), "tech", key);
      }
    }

    for (const auto& eff : t.effects) {
      if (eff.type.empty()) {
        push_error(issues, "tech.effect_empty_type", join("Tech '", key, "' has an effect with empty type"), "tech",
                   key);
        continue;
      }
      if (eff.value.empty()) {
        push_error(issues, "tech.effect_empty_value", join("Tech '", key, "' has an effect with empty value"), "tech",
                   key);
        continue;
      }

      const std::string type = ascii_to_lower(eff.type);
      const std::string val = ascii_to_lower(eff.value);

      if (type == "unlock_component") {
        if (db.components.find(eff.value) == db.components.end()) {
          push_error(issues, "tech.unlock_unknown_component",
                     join("Tech '", key, "' unlocks unknown component '", eff.value, "'"), "tech", key);
        }
      } else if (type == "unlock_installation") {
        if (db.installations.find(eff.value) == db.installations.end()) {
          push_error(issues, "tech.unlock_unknown_installation",
                     join("Tech '", key, "' unlocks unknown installation '", eff.value, "'"), "tech", key);
        }
      } else if (type == "faction_output_bonus" || type == "faction_economy_bonus" || type == "faction_output_multiplier" ||
                 type == "faction_economy_multiplier") {
        if (!is_known_faction_output_key(val)) {
          push_error(issues, "tech.unknown_faction_output_key",
                     join("Tech '", key, "' has faction output effect with unknown key '", eff.value, "'"), "tech",
                     key);
        }
        if (!is_finite(eff.amount)) {
          push_error(issues, "tech.faction_output_invalid_amount",
                     join("Tech '", key, "' has faction output effect with non-finite amount: ", eff.amount), "tech",
                     key);
        } else {
          double factor = 1.0;
          if (type == "faction_output_bonus" || type == "faction_economy_bonus") {
            factor = 1.0 + eff.amount;
          } else {
            factor = eff.amount;
          }
          if (factor <= 0.0) {
            push_warning(issues, "tech.faction_output_nonpositive",
                         join("Tech '", key, "' has faction output effect '", eff.type,
                              "' with non-positive multiplier (", factor, "). Output will clamp to 0."),
                         "tech", key);
          }
        }
      } else {
        // Unknown effect types are effectively ignored by the simulation.
        push_error(issues, "tech.unknown_effect",
                   join("Tech '", key, "' has unknown effect type '", eff.type, "'"), "tech", key);
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
            // Found a back-edge: pre is in current stack.
            const auto pos_it = stack_pos.find(pre);
            if (pos_it == stack_pos.end()) continue;

            const std::size_t start = pos_it->second;
            std::vector<std::string> cycle;
            for (std::size_t i = start; i < stack.size(); ++i) cycle.push_back(stack[i]);
            // Close the cycle.
            cycle.push_back(pre);

            // Canonicalize to avoid duplicates.
            std::vector<std::string> canon = cycle;
            std::sort(canon.begin(), canon.end());
            canon.erase(std::unique(canon.begin(), canon.end()), canon.end());
            std::string key = canon.empty() ? std::string() : canon.front();
            for (std::size_t i = 1; i < canon.size(); ++i) key += "|" + canon[i];

            if (reported.insert(key).second) {
              push_error(issues, "tech.prereq_cycle", join("Tech prerequisite cycle detected: ", key), "tech", id);
            }
          }
        }
      }

      // Pop.
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

  // --- Lints / warnings (non-fatal) ---
  //
  // These are intentionally warnings (not errors) because some scenarios/mods might
  // intentionally unlock content through non-tech mechanisms (starting designs,
  // salvage/reverse engineering, scripted events).
  {
    std::unordered_set<std::string> unlocked_components;
    std::unordered_set<std::string> unlocked_installations;

    for (const auto& [tid, t] : db.techs) {
      for (const auto& eff : t.effects) {
        if (eff.type == "unlock_component" && !eff.value.empty()) unlocked_components.insert(eff.value);
        if (eff.type == "unlock_installation" && !eff.value.empty()) unlocked_installations.insert(eff.value);
      }
    }

    if (!db.techs.empty()) {
      for (const auto& [cid, _] : db.components) {
        if (cid.empty()) continue;
        if (unlocked_components.find(cid) != unlocked_components.end()) continue;
        push_warning(issues, "lint.component_not_unlockable",
                     join("Component '", cid,
                          "' is never unlocked by any tech effect (unlock_component). It may be unreachable via "
                          "research."),
                     "component", cid);
      }

      for (const auto& [iid, _] : db.installations) {
        if (iid.empty()) continue;
        if (unlocked_installations.find(iid) != unlocked_installations.end()) continue;
        push_warning(issues, "lint.installation_not_unlockable",
                     join("Installation '", iid,
                          "' is never unlocked by any tech effect (unlock_installation). It may be unreachable via "
                          "research."),
                     "installation", iid);
      }
    }
  }

  // Sort so output is stable for tests/CI.
  std::sort(issues.begin(), issues.end(), [](const ContentIssue& a, const ContentIssue& b) {
    if (a.severity != b.severity) return a.severity < b.severity; // Error before Warning (enum order)
    if (a.subject_kind != b.subject_kind) return a.subject_kind < b.subject_kind;
    if (a.subject_id != b.subject_id) return a.subject_id < b.subject_id;
    if (a.code != b.code) return a.code < b.code;
    return a.message < b.message;
  });

  return issues;
}

std::vector<std::string> validate_content_db(const ContentDB& db) {
  const auto issues = validate_content_db_detailed(db);

  std::vector<std::string> errors;
  errors.reserve(issues.size());
  for (const auto& is : issues) {
    if (is.severity != ContentIssueSeverity::Error) continue;
    errors.push_back(is.message);
  }

  // Preserve legacy behaviour: stable ordering for tests/CI.
  std::sort(errors.begin(), errors.end());
  return errors;
}

} // namespace nebula4x
