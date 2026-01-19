#include "nebula4x/core/design_stats.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

namespace nebula4x {

DesignDeriveResult derive_ship_design_stats(const ContentDB& content, ShipDesign& design) {
  // Reset derived fields.
  design.mass_tons = 0.0;
  design.speed_km_s = 0.0;
  design.fuel_capacity_tons = 0.0;
  design.fuel_use_per_mkm = 0.0;
  design.cargo_tons = 0.0;
  design.mining_tons_per_day = 0.0;
  design.sensor_range_mkm = 0.0;
  design.signature_multiplier = 1.0;
  design.ecm_strength = 0.0;
  design.eccm_strength = 0.0;
  design.colony_capacity_millions = 0.0;
  design.troop_capacity = 0.0;

  design.power_generation = 0.0;
  design.power_use_total = 0.0;
  design.power_use_engines = 0.0;
  design.power_use_sensors = 0.0;
  design.power_use_weapons = 0.0;
  design.power_use_shields = 0.0;

  design.max_hp = 0.0;
  design.max_shields = 0.0;
  design.shield_regen_per_day = 0.0;

  design.heat_capacity_bonus = 0.0;
  design.heat_generation_bonus_per_day = 0.0;
  design.heat_dissipation_bonus_per_day = 0.0;

  design.weapon_damage = 0.0;
  design.weapon_range_mkm = 0.0;

  design.missile_damage = 0.0;
  design.missile_range_mkm = 0.0;
  design.missile_speed_mkm_per_day = 0.0;
  design.missile_reload_days = 0.0;
  design.missile_launcher_count = 0;
  design.missile_ammo_capacity = 0;

  design.point_defense_damage = 0.0;
  design.point_defense_range_mkm = 0.0;

  // Accumulators.
  double sig_mult = 1.0;
  double hp_bonus = 0.0;

  // Missile weapons.
  double missile_reload = 0.0;
  bool missile_reload_set = false;
  int missile_launcher_count = 0;
  int missile_ammo_capacity = 0;
  bool missile_ammo_finite = true;

  // Thermal.
  double heat_capacity = 0.0;
  double heat_gen = 0.0;
  double heat_diss = 0.0;

  for (const auto& cid : design.components) {
    auto it = content.components.find(cid);
    if (it == content.components.end()) {
      return DesignDeriveResult{false, std::string("Unknown component id: ") + cid};
    }
    const auto& c = it->second;

    design.mass_tons += c.mass_tons;
    design.speed_km_s = std::max(design.speed_km_s, c.speed_km_s);
    design.fuel_capacity_tons += c.fuel_capacity_tons;
    design.fuel_use_per_mkm += c.fuel_use_per_mkm;
    design.cargo_tons += c.cargo_tons;
    design.mining_tons_per_day += c.mining_tons_per_day;
    design.sensor_range_mkm = std::max(design.sensor_range_mkm, c.sensor_range_mkm);
    design.colony_capacity_millions += c.colony_capacity_millions;
    design.troop_capacity += c.troop_capacity;

    design.ecm_strength += std::max(0.0, c.ecm_strength);
    design.eccm_strength += std::max(0.0, c.eccm_strength);

    const double comp_sig =
        std::clamp(std::isfinite(c.signature_multiplier) ? c.signature_multiplier : 1.0, 0.0, 1.0);
    sig_mult *= comp_sig;

    if (c.type == ComponentType::Weapon) {
      design.weapon_damage += c.weapon_damage;
      design.weapon_range_mkm = std::max(design.weapon_range_mkm, c.weapon_range_mkm);

      // Missile launcher stats (optional).
      if (c.missile_damage > 0.0) {
        design.missile_damage += c.missile_damage;
        missile_launcher_count += 1;
        if (c.missile_ammo > 0) {
          missile_ammo_capacity += c.missile_ammo;
        } else {
          // Legacy / unlimited launcher: disable ammo tracking for this design.
          missile_ammo_finite = false;
        }
        design.missile_range_mkm = std::max(design.missile_range_mkm, c.missile_range_mkm);
        design.missile_speed_mkm_per_day = std::max(design.missile_speed_mkm_per_day, c.missile_speed_mkm_per_day);
        if (c.missile_reload_days > 0.0) {
          missile_reload = missile_reload_set ? std::min(missile_reload, c.missile_reload_days) : c.missile_reload_days;
          missile_reload_set = true;
        }
      }

      // Point defense stats (optional).
      if (c.point_defense_damage > 0.0) {
        design.point_defense_damage += c.point_defense_damage;
        design.point_defense_range_mkm = std::max(design.point_defense_range_mkm, c.point_defense_range_mkm);
      }
    }

    if (c.type == ComponentType::Reactor) {
      design.power_generation += c.power_output;
    }
    design.power_use_total += c.power_use;
    if (c.type == ComponentType::Engine) design.power_use_engines += c.power_use;
    if (c.type == ComponentType::Sensor) design.power_use_sensors += c.power_use;
    if (c.type == ComponentType::Weapon) design.power_use_weapons += c.power_use;
    if (c.type == ComponentType::Shield) design.power_use_shields += c.power_use;

    hp_bonus += c.hp_bonus;

    if (c.type == ComponentType::Shield) {
      design.max_shields += c.shield_hp;
      design.shield_regen_per_day += c.shield_regen_per_day;
    }

    // Thermal contributions from components.
    heat_capacity += c.heat_capacity;
    heat_gen += c.heat_generation_per_day;
    heat_diss += c.heat_dissipation_per_day;
  }

  // Clamp to avoid fully-undetectable ships.
  design.signature_multiplier = std::clamp(sig_mult, 0.05, 1.0);
  design.ecm_strength = std::max(0.0, design.ecm_strength);
  design.eccm_strength = std::max(0.0, design.eccm_strength);

  design.missile_launcher_count = missile_launcher_count;
  design.missile_ammo_capacity = (missile_launcher_count > 0 && missile_ammo_finite) ? missile_ammo_capacity : 0;
  design.missile_reload_days = missile_reload_set ? missile_reload : 0.0;

  design.heat_capacity_bonus = std::max(0.0, heat_capacity);
  design.heat_generation_bonus_per_day = std::max(0.0, heat_gen);
  design.heat_dissipation_bonus_per_day = std::max(0.0, heat_diss);

  // Very rough survivability model for the prototype.
  // (Later you can split this into armor/structure/etc.)
  design.max_hp = std::max(1.0, design.mass_tons * 2.0 + hp_bonus);

  return DesignDeriveResult{true, {}};
}

}  // namespace nebula4x
