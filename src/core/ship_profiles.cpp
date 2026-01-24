#include "nebula4x/core/ship_profiles.h"

#include <algorithm>
#include <cmath>

namespace nebula4x {
namespace {

double sane_fraction(double v, double def) {
  if (!std::isfinite(v)) return def;
  return std::clamp(v, 0.0, 1.0);
}

void sanitize_doctrine(ShipCombatDoctrine& d) {
  d.range_fraction = sane_fraction(d.range_fraction, 0.9);
  if (!std::isfinite(d.min_range_mkm) || d.min_range_mkm < 0.0) d.min_range_mkm = 0.0;
  if (!std::isfinite(d.custom_range_mkm) || d.custom_range_mkm < 0.0) d.custom_range_mkm = 0.0;
  d.kite_deadband_fraction = std::clamp(sane_fraction(d.kite_deadband_fraction, 0.10), 0.0, 0.90);
}

}  // namespace

ShipAutomationProfile make_ship_profile_from_ship(const Ship& sh) {
  ShipAutomationProfile p;

  // Mission automation flags.
  p.auto_explore = sh.auto_explore;
  p.auto_freight = sh.auto_freight;
  p.auto_troop_transport = sh.auto_troop_transport;
  p.auto_colonist_transport = sh.auto_colonist_transport;
  p.auto_salvage = sh.auto_salvage;
  p.auto_mine = sh.auto_mine;
  p.auto_mine_home_colony_id = sh.auto_mine_home_colony_id;
  p.auto_mine_mineral = sh.auto_mine_mineral;
  p.auto_colonize = sh.auto_colonize;

  // Sustainment.
  p.auto_refuel = sh.auto_refuel;
  p.auto_refuel_threshold_fraction = sh.auto_refuel_threshold_fraction;
  p.auto_tanker = sh.auto_tanker;
  p.auto_tanker_reserve_fraction = sh.auto_tanker_reserve_fraction;
  p.auto_repair = sh.auto_repair;
  p.auto_repair_threshold_fraction = sh.auto_repair_threshold_fraction;
  p.auto_rearm = sh.auto_rearm;
  p.auto_rearm_threshold_fraction = sh.auto_rearm_threshold_fraction;
  p.repair_priority = sh.repair_priority;

  // Policy.
  p.power_policy = sh.power_policy;
  p.sensor_mode = sh.sensor_mode;
  p.combat_doctrine = sh.combat_doctrine;

  // Keep profiles tidy/sane.
  sanitize_power_policy(p.power_policy);
  sanitize_doctrine(p.combat_doctrine);
  p.auto_refuel_threshold_fraction = sane_fraction(p.auto_refuel_threshold_fraction, 0.25);
  p.auto_tanker_reserve_fraction = sane_fraction(p.auto_tanker_reserve_fraction, 0.25);
  p.auto_repair_threshold_fraction = sane_fraction(p.auto_repair_threshold_fraction, 0.75);
  p.auto_rearm_threshold_fraction = sane_fraction(p.auto_rearm_threshold_fraction, 0.25);

  return p;
}

void apply_ship_profile(Ship& sh, const ShipAutomationProfile& p, const ShipProfileApplyOptions& opt) {
  if (opt.apply_automation) {
    sh.auto_explore = p.auto_explore;
    sh.auto_freight = p.auto_freight;
    sh.auto_troop_transport = p.auto_troop_transport;
    sh.auto_colonist_transport = p.auto_colonist_transport;
    sh.auto_salvage = p.auto_salvage;
    sh.auto_mine = p.auto_mine;
    sh.auto_mine_home_colony_id = p.auto_mine_home_colony_id;
    sh.auto_mine_mineral = p.auto_mine_mineral;
    sh.auto_colonize = p.auto_colonize;

    sh.auto_refuel = p.auto_refuel;
    sh.auto_refuel_threshold_fraction = sane_fraction(p.auto_refuel_threshold_fraction, sh.auto_refuel_threshold_fraction);

    sh.auto_tanker = p.auto_tanker;
    sh.auto_tanker_reserve_fraction = sane_fraction(p.auto_tanker_reserve_fraction, sh.auto_tanker_reserve_fraction);

    sh.auto_repair = p.auto_repair;
    sh.auto_repair_threshold_fraction = sane_fraction(p.auto_repair_threshold_fraction, sh.auto_repair_threshold_fraction);

    sh.auto_rearm = p.auto_rearm;
    sh.auto_rearm_threshold_fraction = sane_fraction(p.auto_rearm_threshold_fraction, sh.auto_rearm_threshold_fraction);
  }

  if (opt.apply_repair_priority) {
    sh.repair_priority = p.repair_priority;
  }

  if (opt.apply_power_policy) {
    sh.power_policy = p.power_policy;
    sanitize_power_policy(sh.power_policy);
  }

  if (opt.apply_sensor_mode) {
    sh.sensor_mode = p.sensor_mode;
  }

  if (opt.apply_combat_doctrine) {
    sh.combat_doctrine = p.combat_doctrine;
    sanitize_doctrine(sh.combat_doctrine);
  }
}

}  // namespace nebula4x
