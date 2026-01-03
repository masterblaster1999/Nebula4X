#pragma once

#include <string>

namespace nebula4x {

struct SimConfig;

enum class GroundBattleWinner {
  Attacker,
  Defender,
};

// Safety guards controlling the battle forecast.
struct GroundBattleForecastOptions {
  // Maximum simulated days before giving up.
  int max_days{3650};
};

// Result of a best-effort battle forecast.
struct GroundBattleForecast {
  bool ok{false};

  // True if the forecast hit max_days before resolving.
  bool truncated{false};
  std::string truncated_reason;

  // Snapshot of the inputs used.
  double attacker_start{0.0};
  double defender_start{0.0};
  double fort_points{0.0};
  double defense_bonus{1.0};

  // Days simulated until resolution.
  // 0 means "already resolved" (one side starts dead).
  int days_to_resolve{0};
  GroundBattleWinner winner{GroundBattleWinner::Defender};

  double attacker_end{0.0};
  double defender_end{0.0};
};

// Forecast the outcome and duration of a ground battle.
//
// - attacker_strength/defender_strength are "strength" points.
// - fort_points are the sum of InstallationDef::fortification_points on the colony.
GroundBattleForecast forecast_ground_battle(const SimConfig& cfg,
                                            double attacker_strength,
                                            double defender_strength,
                                            double fort_points,
                                            const GroundBattleForecastOptions& opt = {});

// Quick analytic estimator based on the (continuous) Lanchester square law.
//
// The simulation is discrete (day steps) and biased toward the defender when both
// sides hit zero on the same day. Treat this as a baseline, not a guarantee.
//
// margin_factor lets UIs request "a bit more" than the theoretical minimum.
double square_law_required_attacker_strength(const SimConfig& cfg,
                                            double defender_strength,
                                            double fort_points,
                                            double margin_factor = 1.0);

const char* ground_battle_winner_label(GroundBattleWinner w);

} // namespace nebula4x
