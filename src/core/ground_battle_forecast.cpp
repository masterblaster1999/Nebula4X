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
                                            double margin_factor) {
  const double def = clamp_nonneg(defender_strength);
  const double forts = clamp_nonneg(fort_points);
  const double fort_scale = std::max(0.0, cfg.fortification_defense_scale);
  const double bonus = std::max(0.0, 1.0 + forts * fort_scale);

  // Continuous square-law threshold: A0 > sqrt(B) * D0.
  const double base = std::sqrt(bonus) * def;
  const double m = std::max(0.0, margin_factor);
  return base * m;
}

GroundBattleForecast forecast_ground_battle(const SimConfig& cfg, double attacker_strength, double defender_strength,
                                            double fort_points, const GroundBattleForecastOptions& opt) {
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
  const double fort_scale = std::max(0.0, cfg.fortification_defense_scale);
  const double defense_bonus = 1.0 + forts * fort_scale;
  out.defense_bonus = defense_bonus;

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
    double attacker_loss = loss_factor * def;
    double defender_loss = (defense_bonus > 1e-9) ? (loss_factor * att / defense_bonus)
                                                 : (loss_factor * att);

    attacker_loss = std::min(attacker_loss, att);
    defender_loss = std::min(defender_loss, def);

    att = clamp_nonneg(att - attacker_loss);
    def = clamp_nonneg(def - defender_loss);

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
