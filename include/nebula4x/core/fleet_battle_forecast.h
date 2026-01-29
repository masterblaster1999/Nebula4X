#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "nebula4x/core/ids.h"

namespace nebula4x {

class Simulation;

// How incoming damage is distributed across targets.
enum class FleetBattleDamageModel : std::uint8_t {
  // Concentrate damage to kill ships quickly (pessimistic for the defender,
  // optimistic for the attacker; tends to produce higher loss rates).
  FocusFire = 0,

  // Spread damage evenly across all targets (optimistic for the defender,
  // pessimistic for the attacker; tends to produce fewer losses).
  EvenSpread = 1,
};

// How engagement range is modeled.
enum class FleetBattleRangeModel : std::uint8_t {
  // Assume both sides are able to apply all weapon systems from t=0.
  //
  // Useful as a "knife-fight" baseline that ignores approach/kiting and
  // sensor constraints.
  Instant = 0,

  // Start inside the longer-range side's envelope and allow closing based on
  // relative average fleet speed. Weapon systems apply only when their range
  // permits.
  //
  // This is still a simplification; treat as a planning aid, not a guarantee.
  RangeAdvantage = 1,
};

enum class FleetBattleWinner : std::uint8_t {
  Attacker = 0,
  Defender = 1,
  Draw = 2,
};

// Safety guards + modeling toggles controlling the battle forecast.
struct FleetBattleForecastOptions {
  // Maximum simulated days before giving up.
  int max_days{60};

  // Fixed time-step in days. Smaller values capture missile salvos / shield regen
  // more smoothly, but cost more CPU.
  double dt_days{0.25};

  FleetBattleDamageModel damage_model{FleetBattleDamageModel::FocusFire};
  FleetBattleRangeModel range_model{FleetBattleRangeModel::Instant};

  // Feature toggles (useful for isolating effects in the UI).
  bool include_beams{true};
  bool include_missiles{true};
  bool include_point_defense{true};
  bool include_shields{true};
  bool include_shield_regen{true};

  // When true, record per-step time series (HP, ship counts, separation).
  bool record_timeline{true};
};

// Aggregated per-side snapshot.
struct FleetSideForecastSummary {
  int start_ships{0};
  int end_ships{0};
  int ships_lost{0};

  // Starting totals.
  double start_hp{0.0};
  double start_shields{0.0};

  // Ending totals.
  double end_hp{0.0};
  double end_shields{0.0};

  // Approximate per-day capacities (pre-range gating).
  double beam_damage_per_day{0.0};
  double missile_salvo_damage{0.0};     // damage per salvo per ship summed (for UI context)
  double missile_reload_days_avg{0.0};  // average reload among ships with missiles
  double point_defense_damage_per_day{0.0};
  double shield_regen_per_day{0.0};

  // Movement / reach.
  double avg_speed_km_s{0.0};
  double max_beam_range_mkm{0.0};
  double max_missile_range_mkm{0.0};
};

// Result of a best-effort fleet battle forecast.
struct FleetBattleForecast {
  bool ok{false};

  // True if the forecast hit max_days before resolving.
  bool truncated{false};
  std::string message;

  // Days simulated until resolution (0 means "already resolved").
  double days_simulated{0.0};
  FleetBattleWinner winner{FleetBattleWinner::Draw};

  // Ending separation for range-model runs (0 for Instant).
  double final_separation_mkm{0.0};

  FleetSideForecastSummary attacker;
  FleetSideForecastSummary defender;

  // Optional time series (length = steps+1).
  std::vector<double> attacker_effective_hp;
  std::vector<double> defender_effective_hp;
  std::vector<int> attacker_ships;
  std::vector<int> defender_ships;
  std::vector<double> separation_mkm;
};

// Forecast a battle between two arbitrary ship lists.
FleetBattleForecast forecast_fleet_battle(const Simulation& sim,
                                          const std::vector<Id>& attacker_ship_ids,
                                          const std::vector<Id>& defender_ship_ids,
                                          const FleetBattleForecastOptions& opt = {});

// Convenience wrapper to forecast a battle between two fleets by id.
FleetBattleForecast forecast_fleet_battle_fleets(const Simulation& sim,
                                                 Id attacker_fleet_id,
                                                 Id defender_fleet_id,
                                                 const FleetBattleForecastOptions& opt = {});

}  // namespace nebula4x
