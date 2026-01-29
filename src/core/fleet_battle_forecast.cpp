#include "nebula4x/core/fleet_battle_forecast.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "nebula4x/core/simulation.h"

namespace nebula4x {

namespace {

constexpr double kSecondsPerDay = 86400.0;
constexpr double kKmPerMkm = 1e6;

double clamp_finite(double v, double lo, double hi, double fallback) {
  if (!std::isfinite(v)) return fallback;
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

struct Unit {
  Id ship_id{kInvalidId};

  // State.
  double hp{0.0};
  double shields{0.0};

  // Caps.
  double max_hp{0.0};
  double max_shields{0.0};

  // Defensive regen.
  double shield_regen_per_day{0.0};

  // Offensive (already includes crew+subsystem multiplier).
  double beam_damage_per_day{0.0};
  double beam_range_mkm{0.0};

  double missile_damage_per_salvo{0.0};
  double missile_range_mkm{0.0};
  double missile_reload_days{0.0};
  int missile_ammo{-1};  // -1 => infinite/unbounded
  double missile_timer_days{0.0};

  double point_defense_damage_per_day{0.0};

  // Kinematics.
  double speed_km_s{0.0};
};

double unit_effective_hp(const Unit& u, bool include_shields) {
  const double h = std::max(0.0, u.hp);
  const double s = include_shields ? std::max(0.0, u.shields) : 0.0;
  return h + s;
}

double side_effective_hp(const std::vector<Unit>& units, bool include_shields) {
  double sum = 0.0;
  for (const auto& u : units) sum += unit_effective_hp(u, include_shields);
  return sum;
}

int side_alive_count(const std::vector<Unit>& units) {
  int n = 0;
  for (const auto& u : units) {
    if (u.hp > 0.0) ++n;
  }
  return n;
}

double max_side_engagement_range_mkm(const std::vector<Unit>& units) {
  double r = 0.0;
  for (const auto& u : units) {
    r = std::max(r, u.beam_range_mkm);
    r = std::max(r, u.missile_range_mkm);
  }
  if (!std::isfinite(r) || r < 0.0) r = 0.0;
  return r;
}

double avg_side_speed_km_s(const std::vector<Unit>& units) {
  double sum = 0.0;
  int n = 0;
  for (const auto& u : units) {
    if (u.speed_km_s > 0.0 && std::isfinite(u.speed_km_s)) {
      sum += u.speed_km_s;
      ++n;
    }
  }
  if (n <= 0) return 0.0;
  return sum / static_cast<double>(n);
}

double km_s_to_mkm_per_day(double km_s) {
  if (!std::isfinite(km_s) || km_s <= 0.0) return 0.0;
  return (km_s * kSecondsPerDay) / kKmPerMkm;
}

void regen_shields(std::vector<Unit>& units, double dt_days, bool enabled) {
  if (!enabled) return;
  for (auto& u : units) {
    if (u.hp <= 0.0) continue;
    if (u.max_shields <= 0.0) continue;
    const double regen = std::max(0.0, u.shield_regen_per_day);
    if (regen <= 0.0) continue;
    u.shields = std::min(u.max_shields, std::max(0.0, u.shields) + regen * dt_days);
  }
}

// Apply damage to shields first (if enabled), then HP.
// Returns leftover damage if the unit died before consuming the assigned damage.
double apply_damage_to_unit(Unit& u, double dmg, bool include_shields) {
  if (u.hp <= 0.0) return dmg;
  dmg = std::max(0.0, dmg);
  if (dmg <= 0.0) return 0.0;

  if (include_shields && u.shields > 0.0) {
    const double ds = std::min(u.shields, dmg);
    u.shields -= ds;
    dmg -= ds;
  }
  if (dmg > 0.0 && u.hp > 0.0) {
    const double dh = std::min(u.hp, dmg);
    u.hp -= dh;
    dmg -= dh;
  }

  if (u.hp <= 0.0) {
    u.hp = 0.0;
    u.shields = 0.0;
  }
  return dmg;
}

void compact_dead(std::vector<Unit>& units) {
  units.erase(std::remove_if(units.begin(), units.end(), [](const Unit& u) { return u.hp <= 0.0; }), units.end());
}

// Focus-fire model: kill the lowest-effective-HP ship first.
void apply_damage_focus(std::vector<Unit>& targets, double dmg, bool include_shields) {
  dmg = std::max(0.0, dmg);
  while (dmg > 1e-9 && !targets.empty()) {
    // pick lowest effective HP.
    auto it = std::min_element(targets.begin(), targets.end(), [&](const Unit& a, const Unit& b) {
      return unit_effective_hp(a, include_shields) < unit_effective_hp(b, include_shields);
    });
    if (it == targets.end()) break;

    dmg = apply_damage_to_unit(*it, dmg, include_shields);
    if (it->hp <= 0.0) {
      // remove (swap+pop for O(1))
      *it = targets.back();
      targets.pop_back();
    }
  }
}

// Even-spread model: distribute damage equally; redistribute leftover if ships die.
void apply_damage_even(std::vector<Unit>& targets, double dmg, bool include_shields) {
  dmg = std::max(0.0, dmg);
  int safety = 0;
  while (dmg > 1e-9 && !targets.empty() && safety < 64) {
    ++safety;
    const double share = dmg / static_cast<double>(targets.size());
    double leftover = 0.0;
    for (auto& t : targets) {
      leftover += apply_damage_to_unit(t, share, include_shields);
    }
    compact_dead(targets);
    dmg = leftover;
  }
}

void apply_damage(std::vector<Unit>& targets, double dmg, FleetBattleDamageModel mode, bool include_shields) {
  switch (mode) {
    case FleetBattleDamageModel::EvenSpread:
      apply_damage_even(targets, dmg, include_shields);
      return;
    case FleetBattleDamageModel::FocusFire:
    default:
      apply_damage_focus(targets, dmg, include_shields);
      return;
  }
}

struct StepFireResult {
  double beam_damage{0.0};
  double missile_damage{0.0};
};

// Compute outgoing damage (beam + missile) for a side.
// Mutates missile timers/ammo.
// Range gating is applied when range_model == RangeAdvantage.
StepFireResult compute_side_fire(std::vector<Unit>& side,
                                double dt_days,
                                FleetBattleRangeModel range_model,
                                double separation_mkm,
                                bool include_beams,
                                bool include_missiles) {
  StepFireResult out;
  for (auto& u : side) {
    if (u.hp <= 0.0) continue;

    // Beams
    if (include_beams && u.beam_damage_per_day > 0.0) {
      const bool in_range = (range_model == FleetBattleRangeModel::Instant) ||
                            (u.beam_range_mkm > 0.0 && separation_mkm <= u.beam_range_mkm);
      if (in_range) out.beam_damage += u.beam_damage_per_day * dt_days;
    }

    // Missiles (discrete salvos)
    if (include_missiles && u.missile_damage_per_salvo > 0.0 && u.missile_reload_days > 1e-6) {
      const bool in_range = (range_model == FleetBattleRangeModel::Instant) ||
                            (u.missile_range_mkm > 0.0 && separation_mkm <= u.missile_range_mkm);
      if (!in_range) continue;

      // Advance timer.
      u.missile_timer_days -= dt_days;

      // Fire as many salvos as fit in this step.
      int safety = 0;
      while (u.missile_timer_days <= 1e-9 && safety < 32) {
        ++safety;
        if (u.missile_ammo == 0) {
          // Out of ammo; keep timer at 0 to indicate "ready but empty".
          u.missile_timer_days = 0.0;
          break;
        }

        out.missile_damage += u.missile_damage_per_salvo;

        if (u.missile_ammo > 0) --u.missile_ammo;

        u.missile_timer_days += u.missile_reload_days;
      }

      // Clamp.
      if (!std::isfinite(u.missile_timer_days) || u.missile_timer_days < 0.0) u.missile_timer_days = 0.0;
    }
  }
  return out;
}

double compute_pd_capacity(const std::vector<Unit>& side, double dt_days) {
  double pd = 0.0;
  for (const auto& u : side) {
    if (u.hp <= 0.0) continue;
    if (u.point_defense_damage_per_day > 0.0) pd += u.point_defense_damage_per_day * dt_days;
  }
  if (!std::isfinite(pd) || pd < 0.0) pd = 0.0;
  return pd;
}

bool side_can_project_damage(const std::vector<Unit>& side, FleetBattleRangeModel range_model, double separation_mkm) {
  for (const auto& u : side) {
    if (u.hp <= 0.0) continue;
    if (u.beam_damage_per_day > 0.0) {
      if (range_model == FleetBattleRangeModel::Instant) return true;
      if (u.beam_range_mkm > 0.0 && separation_mkm <= u.beam_range_mkm) return true;
    }
    if (u.missile_damage_per_salvo > 0.0 && u.missile_reload_days > 1e-6) {
      if (range_model == FleetBattleRangeModel::Instant) return true;
      if (u.missile_range_mkm > 0.0 && separation_mkm <= u.missile_range_mkm) return true;
    }
  }
  return false;
}

// Update separation in the RangeAdvantage model.
double update_separation_range_advantage(const std::vector<Unit>& attacker,
                                        const std::vector<Unit>& defender,
                                        double separation_mkm,
                                        double dt_days) {
  separation_mkm = std::max(0.0, separation_mkm);

  const bool att_can = side_can_project_damage(attacker, FleetBattleRangeModel::RangeAdvantage, separation_mkm);
  const bool def_can = side_can_project_damage(defender, FleetBattleRangeModel::RangeAdvantage, separation_mkm);

  // If both can engage (or neither can), keep separation constant.
  if (att_can == def_can) return separation_mkm;

  // The out-of-range side closes; the in-range side may kite.
  const double att_speed = km_s_to_mkm_per_day(avg_side_speed_km_s(attacker));
  const double def_speed = km_s_to_mkm_per_day(avg_side_speed_km_s(defender));

  const bool attacker_closing = !att_can && def_can;
  const double closing_speed = attacker_closing ? att_speed : def_speed;
  const double kiting_speed = attacker_closing ? def_speed : att_speed;

  if (closing_speed <= 1e-9) return separation_mkm;

  // If the in-range side is about as fast or faster, assume it can keep distance.
  if (kiting_speed >= closing_speed * 0.98) return separation_mkm;

  const double rel = std::max(0.0, closing_speed - kiting_speed);
  const double new_sep = std::max(0.0, separation_mkm - rel * dt_days);
  return new_sep;
}

FleetSideForecastSummary summarize_side_start(const std::vector<Unit>& units) {
  FleetSideForecastSummary s;
  s.start_ships = static_cast<int>(units.size());
  s.end_ships = s.start_ships;
  s.ships_lost = 0;

  for (const auto& u : units) {
    s.start_hp += std::max(0.0, u.hp);
    s.start_shields += std::max(0.0, u.shields);
    s.beam_damage_per_day += std::max(0.0, u.beam_damage_per_day);
    s.point_defense_damage_per_day += std::max(0.0, u.point_defense_damage_per_day);
    s.shield_regen_per_day += std::max(0.0, u.shield_regen_per_day);
    s.max_beam_range_mkm = std::max(s.max_beam_range_mkm, std::max(0.0, u.beam_range_mkm));
    s.max_missile_range_mkm = std::max(s.max_missile_range_mkm, std::max(0.0, u.missile_range_mkm));
    s.avg_speed_km_s += std::max(0.0, u.speed_km_s);

    if (u.missile_damage_per_salvo > 0.0 && u.missile_reload_days > 1e-6) {
      s.missile_salvo_damage += std::max(0.0, u.missile_damage_per_salvo);
      s.missile_reload_days_avg += u.missile_reload_days;
    }
  }

  if (s.start_ships > 0) {
    s.avg_speed_km_s /= static_cast<double>(s.start_ships);
  }
  // missile_reload_days_avg currently sums; normalize by missile-capable ships
  if (s.missile_salvo_damage > 0.0) {
    // Count missile ships by reconstructing (cheap enough for summary).
    int missile_n = 0;
    for (const auto& u : units) {
      if (u.missile_damage_per_salvo > 0.0 && u.missile_reload_days > 1e-6) ++missile_n;
    }
    if (missile_n > 0) s.missile_reload_days_avg /= static_cast<double>(missile_n);
  } else {
    s.missile_reload_days_avg = 0.0;
  }

  return s;
}

void summarize_side_end(FleetSideForecastSummary& s, const std::vector<Unit>& units) {
  s.end_ships = static_cast<int>(units.size());
  s.ships_lost = std::max(0, s.start_ships - s.end_ships);

  s.end_hp = 0.0;
  s.end_shields = 0.0;
  for (const auto& u : units) {
    s.end_hp += std::max(0.0, u.hp);
    s.end_shields += std::max(0.0, u.shields);
  }
}

}  // namespace

FleetBattleForecast forecast_fleet_battle(const Simulation& sim,
                                          const std::vector<Id>& attacker_ship_ids,
                                          const std::vector<Id>& defender_ship_ids,
                                          const FleetBattleForecastOptions& opt) {
  FleetBattleForecast out;

  const int max_days = std::max(0, opt.max_days);
  double dt = opt.dt_days;
  if (!std::isfinite(dt) || dt <= 1e-6) dt = 0.25;
  dt = std::clamp(dt, 1e-3, 10.0);

  // Gather and validate ships.
  auto build_units = [&](const std::vector<Id>& ship_ids, std::vector<Unit>* dst, std::string* err) -> bool {
    dst->clear();
    dst->reserve(ship_ids.size());

    for (Id sid : ship_ids) {
      const Ship* ship = find_ptr(sim.state().ships, sid);
      if (!ship) continue;

      const ShipDesign* d = sim.find_design(ship->design_id);
      if (!d) continue;

      // Defensive stats.
      const double max_hp = std::max(0.0, d->max_hp);
      const double max_sh = std::max(0.0, d->max_shields);
      // Multipliers. These are applied to approximate the effects of crew quality and
      // subsystem damage without running the full combat simulation.
      const double crew_mult = std::max(0.0, 1.0 + sim.crew_grade_bonus(*ship));
      const double shield_mult = std::max(0.0, sim.ship_subsystem_shield_multiplier(*ship));
      const double weapon_mult =
          std::max(0.0, sim.ship_subsystem_weapon_output_multiplier(*ship)) * crew_mult;
      const double engine_mult = std::max(0.0, sim.ship_subsystem_engine_multiplier(*ship));



      double hp = ship->hp;
      if (!std::isfinite(hp) || hp <= 0.0) hp = max_hp;
      hp = std::max(0.0, hp);

      if (hp <= 0.0) continue;

      double sh = ship->shields;
      // In the main sim tick, shields are clamped to max_shields * subsystem multiplier.
      // If shields are uninitialized (<0), start at the effective max.
      if (!std::isfinite(sh) || sh < 0.0) sh = max_sh * shield_mult;
      sh = std::max(0.0, sh);

      Unit u;
      u.ship_id = sid;

      u.max_hp = max_hp;
      u.max_shields = max_sh * shield_mult;

      u.hp = hp;
      // ship->shields is already tracked in "effective" units (clamped against
      // design max_shields * subsystem multiplier in the main sim tick), so do not
      // apply shield_mult again here.
      u.shields = std::min(u.max_shields, sh);

      // Regen.
      u.shield_regen_per_day = std::max(0.0, d->shield_regen_per_day) * shield_mult;

      // Offense.
      u.beam_damage_per_day = std::max(0.0, d->weapon_damage) * weapon_mult;
      u.beam_range_mkm = std::max(0.0, d->weapon_range_mkm);

      u.missile_damage_per_salvo = std::max(0.0, d->missile_damage) * weapon_mult;
      u.missile_range_mkm = std::max(0.0, d->missile_range_mkm);
      u.missile_reload_days = std::max(0.0, d->missile_reload_days);

      u.point_defense_damage_per_day = std::max(0.0, d->point_defense_damage) * weapon_mult;

      // Missile ammo.
      if (d->missile_ammo_capacity <= 0) {
        u.missile_ammo = -1;
      } else {
        int ammo = ship->missile_ammo;
        if (ammo < 0) ammo = d->missile_ammo_capacity;
        ammo = std::clamp(ammo, 0, d->missile_ammo_capacity);
        u.missile_ammo = ammo;
      }

      // Initial missile timer.
      u.missile_timer_days = clamp_finite(ship->missile_cooldown_days, 0.0, 1e9, 0.0);

      // Speed.
      const double base_speed = (std::isfinite(ship->speed_km_s) && ship->speed_km_s > 0.0)
                                     ? ship->speed_km_s
                                     : d->speed_km_s;
      u.speed_km_s = std::max(0.0, base_speed) * engine_mult;

      dst->push_back(u);
    }

    if (dst->empty()) {
      if (err) *err = "no valid ships";
      return false;
    }
    return true;
  };

  std::vector<Unit> attacker;
  std::vector<Unit> defender;

  std::string err;
  if (!build_units(attacker_ship_ids, &attacker, &err)) {
    out.ok = false;
    out.message = "Attacker: " + err;
    return out;
  }
  if (!build_units(defender_ship_ids, &defender, &err)) {
    out.ok = false;
    out.message = "Defender: " + err;
    return out;
  }

  // Initial summaries.
  out.attacker = summarize_side_start(attacker);
  out.defender = summarize_side_start(defender);

  // Quick resolution if one side is already dead.
  if (attacker.empty() || defender.empty()) {
    out.ok = true;
    out.truncated = false;
    out.days_simulated = 0.0;
    out.winner = attacker.empty() ? FleetBattleWinner::Defender : FleetBattleWinner::Attacker;
    summarize_side_end(out.attacker, attacker);
    summarize_side_end(out.defender, defender);
    out.message = "Already resolved";
    return out;
  }

  // Starting separation for range model.
  double separation_mkm = 0.0;
  if (opt.range_model == FleetBattleRangeModel::RangeAdvantage) {
    const double ra = max_side_engagement_range_mkm(attacker);
    const double rd = max_side_engagement_range_mkm(defender);
    separation_mkm = std::max(0.0, std::max(ra, rd) * 0.8);
  }

  // Timeline init.
  if (opt.record_timeline) {
    out.attacker_effective_hp.reserve(static_cast<size_t>(max_days / dt) + 4);
    out.defender_effective_hp.reserve(static_cast<size_t>(max_days / dt) + 4);
    out.attacker_ships.reserve(static_cast<size_t>(max_days / dt) + 4);
    out.defender_ships.reserve(static_cast<size_t>(max_days / dt) + 4);
    out.separation_mkm.reserve(static_cast<size_t>(max_days / dt) + 4);

    out.attacker_effective_hp.push_back(side_effective_hp(attacker, opt.include_shields));
    out.defender_effective_hp.push_back(side_effective_hp(defender, opt.include_shields));
    out.attacker_ships.push_back(static_cast<int>(attacker.size()));
    out.defender_ships.push_back(static_cast<int>(defender.size()));
    out.separation_mkm.push_back(separation_mkm);
  }

  const int max_steps = (dt > 0.0) ? static_cast<int>(std::ceil(static_cast<double>(max_days) / dt)) : 0;
  int steps = 0;

  for (; steps < max_steps; ++steps) {
    // Regen shields.
    if (opt.include_shields && opt.include_shield_regen) {
      regen_shields(attacker, dt, true);
      regen_shields(defender, dt, true);
    }

    // Fire (simultaneous).
    StepFireResult att_fire =
        compute_side_fire(attacker, dt, opt.range_model, separation_mkm, opt.include_beams, opt.include_missiles);
    StepFireResult def_fire =
        compute_side_fire(defender, dt, opt.range_model, separation_mkm, opt.include_beams, opt.include_missiles);

    // Point defense reduces incoming missile damage.
    double att_missile = att_fire.missile_damage;
    double def_missile = def_fire.missile_damage;

    if (opt.include_missiles && opt.include_point_defense) {
      const double def_pd = compute_pd_capacity(defender, dt);
      const double att_pd = compute_pd_capacity(attacker, dt);
      att_missile = std::max(0.0, att_missile - def_pd);
      def_missile = std::max(0.0, def_missile - att_pd);
    }

    const double att_total = std::max(0.0, att_fire.beam_damage) + std::max(0.0, att_missile);
    const double def_total = std::max(0.0, def_fire.beam_damage) + std::max(0.0, def_missile);

    // Apply damage.
    apply_damage(defender, att_total, opt.damage_model, opt.include_shields);
    apply_damage(attacker, def_total, opt.damage_model, opt.include_shields);

    // Check resolution.
    if (attacker.empty() || defender.empty()) break;

    // Range model: update separation after the exchange.
    if (opt.range_model == FleetBattleRangeModel::RangeAdvantage) {
      separation_mkm = update_separation_range_advantage(attacker, defender, separation_mkm, dt);
    }

    // Record timeline.
    if (opt.record_timeline) {
      out.attacker_effective_hp.push_back(side_effective_hp(attacker, opt.include_shields));
      out.defender_effective_hp.push_back(side_effective_hp(defender, opt.include_shields));
      out.attacker_ships.push_back(static_cast<int>(attacker.size()));
      out.defender_ships.push_back(static_cast<int>(defender.size()));
      out.separation_mkm.push_back(separation_mkm);
    }
  }

  out.ok = true;
  out.days_simulated = steps * dt;
  out.final_separation_mkm = (opt.range_model == FleetBattleRangeModel::RangeAdvantage) ? separation_mkm : 0.0;

  summarize_side_end(out.attacker, attacker);
  summarize_side_end(out.defender, defender);

  if (attacker.empty() && defender.empty()) {
    out.winner = FleetBattleWinner::Draw;
  } else if (defender.empty()) {
    out.winner = FleetBattleWinner::Attacker;
  } else if (attacker.empty()) {
    out.winner = FleetBattleWinner::Defender;
  } else {
    // Unresolved within max_days.
    out.truncated = true;
    out.message = "Truncated (max_days reached)";

    const double a = side_effective_hp(attacker, opt.include_shields);
    const double d = side_effective_hp(defender, opt.include_shields);
    if (a > d * 1.01) {
      out.winner = FleetBattleWinner::Attacker;
    } else if (d > a * 1.01) {
      out.winner = FleetBattleWinner::Defender;
    } else {
      out.winner = FleetBattleWinner::Draw;
    }
    return out;
  }

  out.truncated = false;
  out.message = "Resolved";
  return out;
}

FleetBattleForecast forecast_fleet_battle_fleets(const Simulation& sim,
                                                 Id attacker_fleet_id,
                                                 Id defender_fleet_id,
                                                 const FleetBattleForecastOptions& opt) {
  FleetBattleForecast out;
  const Fleet* af = find_ptr(sim.state().fleets, attacker_fleet_id);
  const Fleet* df = find_ptr(sim.state().fleets, defender_fleet_id);
  if (!af || !df) {
    out.ok = false;
    out.message = "Invalid fleet id(s)";
    return out;
  }
  return forecast_fleet_battle(sim, af->ship_ids, df->ship_ids, opt);
}

}  // namespace nebula4x
