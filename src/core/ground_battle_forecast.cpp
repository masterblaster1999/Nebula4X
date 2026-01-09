#include "nebula4x/core/ground_battle_forecast.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

#include "nebula4x/core/simulation.h"

namespace nebula4x {

namespace {

double clamp_nonneg(double x) { return (x < 0.0) ? 0.0 : x; }

} // namespace

const char* ground_battle_winner_label(GroundBattleWinner w) {
  switch (w) {
    case GroundBattleWinner::Attacker:
      return "Attacker";
    case GroundBattleWinner::Defender:
    default:
      return "Defender";
  }
}

double square_law_required_attacker_strength(const SimConfig& cfg, double defender_strength, double fort_points,
                                            double defender_artillery_weapon_damage_per_day, double margin_factor) {
  const double def = clamp_nonneg(defender_strength);
  const double forts = clamp_nonneg(fort_points);
  const double art_weapon = clamp_nonneg(defender_artillery_weapon_damage_per_day);

  const double loss_factor = std::max(0.0, cfg.ground_combat_loss_factor);
  const double fort_def_scale = std::max(0.0, cfg.fortification_defense_scale);
  const double fort_atk_scale = std::max(0.0, cfg.fortification_attack_scale);
  const double arty_strength_per_weapon =
      std::max(0.0, cfg.ground_combat_defender_artillery_strength_per_weapon_damage);

  const double defense_bonus = std::max(0.0, 1.0 + forts * fort_def_scale);
  const double offense_bonus = std::max(0.0, 1.0 + forts * fort_atk_scale);

  // Treat defender artillery as an "equivalent" defender strength term so that
  // this remains a quick analytic estimator.
  double artillery_equiv = 0.0;
  if (loss_factor > 1e-9) {
    artillery_equiv = art_weapon * arty_strength_per_weapon / loss_factor;
  }
  const double def_eff = def + artillery_equiv;

  // Continuous square-law threshold for dA/dt = -k*(D*offense_bonus + art_equiv),
  // dD/dt = -k*A/defense_bonus.
  const double base = std::sqrt(defense_bonus * offense_bonus) * def_eff;
  const double m = std::max(0.0, margin_factor);
  return base * m;
}

GroundBattleForecast forecast_ground_battle(const SimConfig& cfg, double attacker_strength, double defender_strength,
                                            double fort_points, double defender_artillery_weapon_damage_per_day,
                                            const GroundBattleForecastOptions& opt) {
  GroundBattleForecast out;
  out.ok = false;

  if (!std::isfinite(attacker_strength) || !std::isfinite(defender_strength) || !std::isfinite(fort_points)) {
    out.truncated = true;
    out.truncated_reason = "Non-finite inputs";
    return out;
  }

  const int max_days = std::max(0, opt.max_days);

  double att = clamp_nonneg(attacker_strength);
  double def = clamp_nonneg(defender_strength);
  const double forts = clamp_nonneg(fort_points);

  out.attacker_start = att;
  out.defender_start = def;
  out.fort_points = forts;

  const double loss_factor = std::max(0.0, cfg.ground_combat_loss_factor);
  const double fort_def_scale = std::max(0.0, cfg.fortification_defense_scale);
  const double fort_atk_scale = std::max(0.0, cfg.fortification_attack_scale);
  const double arty_strength_per_weapon =
      std::max(0.0, cfg.ground_combat_defender_artillery_strength_per_weapon_damage);
  const double fort_damage_rate =
      std::max(0.0, cfg.ground_combat_fortification_damage_per_attacker_strength_day);

  const double base_arty_weapon = clamp_nonneg(defender_artillery_weapon_damage_per_day);
  double fort_damage = 0.0;

  // Snapshot the initial defender fortification defensive bonus (for UI/debug).
  out.defense_bonus = 1.0 + forts * fort_def_scale;

  constexpr double kEps = 1e-6;

  auto resolve = [&](int days, GroundBattleWinner w) {
    out.ok = true;
    out.days_to_resolve = std::max(0, days);
    out.winner = w;
    out.attacker_end = att;
    out.defender_end = def;
  };

  // If either side is already dead, this resolves immediately.
  const bool attacker_dead0 = (att <= kEps);
  const bool defender_dead0 = (def <= kEps);
  if (defender_dead0 && !attacker_dead0) {
    def = 0.0;
    resolve(0, GroundBattleWinner::Attacker);
    return out;
  }
  if (attacker_dead0) {
    att = 0.0;
    resolve(0, GroundBattleWinner::Defender);
    return out;
  }

  if (max_days == 0) {
    out.ok = true;
    out.truncated = true;
    out.truncated_reason = "max_days == 0";
    out.days_to_resolve = 0;
    out.attacker_end = att;
    out.defender_end = def;
    out.winner = GroundBattleWinner::Defender;
    return out;
  }

  // Mirror Simulation::tick_ground_combat loss model.
  for (int day = 0; day < max_days; ++day) {
    const double eff_forts = std::max(0.0, forts - fort_damage);
    const double defense_bonus = 1.0 + eff_forts * fort_def_scale;
    const double offense_bonus = 1.0 + eff_forts * fort_atk_scale;

    double fort_integrity = 1.0;
    if (forts > 1e-9) fort_integrity = std::clamp(eff_forts / forts, 0.0, 1.0);

    const double arty_weapon = base_arty_weapon * fort_integrity;
    const double arty_loss = arty_weapon * arty_strength_per_weapon;

    double attacker_loss = loss_factor * def * offense_bonus + arty_loss;
    double defender_loss = (defense_bonus > 1e-9)
                               ? (loss_factor * att / defense_bonus)
                               : (loss_factor * att);

    attacker_loss = std::min(attacker_loss, att);
    defender_loss = std::min(defender_loss, def);

    att = clamp_nonneg(att - attacker_loss);
    def = clamp_nonneg(def - defender_loss);

    // Fortification degradation happens alongside combat and uses the remaining
    // attacker strength (matches Simulation::tick_ground_combat).
    if (fort_damage_rate > 1e-9 && forts > 1e-9 && att > 1e-9) {
      fort_damage += att * fort_damage_rate;
      if (fort_damage > forts) fort_damage = forts;
    }

    const int days_fought = day + 1;
    const bool attacker_dead = (att <= kEps);
    const bool defender_dead = (def <= kEps);

    // Resolution ordering matches Simulation::tick_ground_combat:
    // - Defender dead AND attacker alive => capture.
    // - Otherwise if attacker dead => defense holds.
    if (defender_dead && !attacker_dead) {
      def = 0.0;
      resolve(days_fought, GroundBattleWinner::Attacker);
      return out;
    }
    if (attacker_dead) {
      att = 0.0;
      resolve(days_fought, GroundBattleWinner::Defender);
      return out;
    }
  }

  // If we reach here, we didn't resolve within max_days.
  out.ok = true;
  out.truncated = true;
  out.truncated_reason = "Exceeded max_days";
  out.days_to_resolve = max_days;
  out.winner = GroundBattleWinner::Defender;
  out.attacker_end = att;
  out.defender_end = def;
  return out;
}

} // namespace nebula4x
