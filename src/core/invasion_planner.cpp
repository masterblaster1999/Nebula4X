#include "nebula4x/core/invasion_planner.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "nebula4x/core/simulation.h"
#include "nebula4x/util/sorted_keys.h"

namespace nebula4x {
namespace {

using nebula4x::util::sorted_keys;

bool finite_pos(double x) {
  return std::isfinite(x) && x >= 0.0;
}

// Best-effort ETA estimate (days) from a start position to a goal position.
//
// Returns +inf when the route is unreachable under the given constraints.
double estimate_eta_days_to_pos(const Simulation& sim, Id start_system_id, Vec2 start_pos_mkm,
                               Id faction_id, double speed_km_s,
                               Id goal_system_id, Vec2 goal_pos_mkm,
                               bool restrict_to_discovered) {
  if (start_system_id == kInvalidId || goal_system_id == kInvalidId) {
    return std::numeric_limits<double>::infinity();
  }
  if (!(speed_km_s > 1e-9)) {
    return std::numeric_limits<double>::infinity();
  }

  const auto plan = sim.plan_jump_route_from_pos(start_system_id, start_pos_mkm, faction_id,
                                                speed_km_s, goal_system_id,
                                                restrict_to_discovered, goal_pos_mkm);
  if (!plan.has_value()) {
    return std::numeric_limits<double>::infinity();
  }
  return std::max(0.0, plan->total_eta_days);
}

// Sums colony installation weapon damage ("artillery") for ground combat.
//
// Note: simulation_tick_ai uses the same approximation when estimating assault
// requirements.
double colony_artillery_weapon_damage_per_day(const Simulation& sim, const Colony& col) {
  double total = 0.0;
  for (const auto& [inst_id, count] : col.installations) {
    if (count <= 0) continue;
    const auto it = sim.content().installations.find(inst_id);
    if (it == sim.content().installations.end()) continue;
    const double wd = it->second.weapon_damage;
    if (wd <= 1e-9) continue;
    total += wd * static_cast<double>(count);
  }
  return std::max(0.0, total);
}

}  // namespace

InvasionPlannerResult analyze_invasion_target(const Simulation& sim, Id target_colony_id,
                                            const InvasionPlannerOptions& opt,
                                            double troop_margin_factor,
                                            double attacker_strength_for_forecast) {
  InvasionPlannerResult out;

  const GameState& s = sim.state();

  const Colony* tgt = find_ptr(s.colonies, target_colony_id);
  if (!tgt) {
    out.ok = false;
    out.message = "Target colony not found.";
    return out;
  }

  const Body* tgt_body = find_ptr(s.bodies, tgt->body_id);
  if (!tgt_body || tgt_body->system_id == kInvalidId) {
    out.ok = false;
    out.message = "Target colony body/system not found.";
    return out;
  }

  if (opt.attacker_faction_id != kInvalidId && opt.restrict_to_discovered) {
    if (!sim.is_system_discovered_by_faction(opt.attacker_faction_id, tgt_body->system_id)) {
      out.ok = false;
      out.message = "Target system is undiscovered for the attacker.";
      return out;
    }
  }

  // --- Defender snapshot (uses active battle state when present) ---
  double defender_strength = std::max(0.0, tgt->ground_forces);
  double fort_damage_points = 0.0;

  if (auto itb = s.ground_battles.find(tgt->id); itb != s.ground_battles.end()) {
    const auto& b = itb->second;
    // If the colony's current faction is the defender in this battle, the battle
    // record is the authoritative strength snapshot.
    if (b.defender_faction_id == tgt->faction_id) {
      defender_strength = std::max(0.0, b.defender_strength);
      fort_damage_points = std::max(0.0, b.fortification_damage_points);
    }
  }

  // --- Fortifications and artillery ---
  const double forts_total = std::max(0.0, sim.fortification_points(*tgt));
  const double forts_effective = std::max(0.0, forts_total - std::min(forts_total, fort_damage_points));

  const double fort_integrity = (forts_total > 1e-9) ? std::clamp(forts_effective / forts_total, 0.0, 1.0) : 0.0;

  const double arty_base = colony_artillery_weapon_damage_per_day(sim, *tgt);
  const double defender_arty = std::max(0.0, arty_base * fort_integrity);

  // Required attacker strength estimate.
  const double margin = std::clamp(troop_margin_factor, 1.0, 10.0);

  auto forecast_for = [&](double attacker_strength, double forts, double arty) {
    return forecast_ground_battle(sim.cfg(), std::max(0.0, attacker_strength), defender_strength,
                                std::max(0.0, forts), std::max(0.0, arty));
  };

  double required = std::max(0.0,
                             square_law_required_attacker_strength(sim.cfg(), defender_strength,
                                                                 forts_effective, defender_arty, margin));

  GroundBattleForecast req_forecast = forecast_for(required, forts_effective, defender_arty);
  // Best-effort: nudge upward until the analytic forecast predicts an attacker win.
  // This reduces surprises from numerical/edge-case mismatches.
  for (int i = 0; i < 12; ++i) {
    if (req_forecast.winner == GroundBattleWinner::Attacker || defender_strength <= 1e-9) break;
    required *= 1.15;
    req_forecast = forecast_for(required, forts_effective, defender_arty);
  }

  // Alternate "no forts" scenario (fully suppressed defenses).
  double required_nf = std::max(0.0,
                                square_law_required_attacker_strength(sim.cfg(), defender_strength,
                                                                    0.0, 0.0, margin));
  GroundBattleForecast req_nf_forecast = forecast_for(required_nf, 0.0, 0.0);
  for (int i = 0; i < 12; ++i) {
    if (req_nf_forecast.winner == GroundBattleWinner::Attacker || defender_strength <= 1e-9) break;
    required_nf *= 1.15;
    req_nf_forecast = forecast_for(required_nf, 0.0, 0.0);
  }

  out.target.colony_id = tgt->id;
  out.target.system_id = tgt_body->system_id;
  out.target.defender_faction_id = tgt->faction_id;
  out.target.defender_strength = defender_strength;
  out.target.forts_total = forts_total;
  out.target.forts_effective = forts_effective;
  out.target.fort_damage_points = fort_damage_points;
  out.target.defender_artillery_weapon_damage_per_day = defender_arty;
  out.target.required_attacker_strength = required;
  out.target.forecast_at_required = req_forecast;
  out.target.required_attacker_strength_no_forts = required_nf;
  out.target.forecast_at_required_no_forts = req_nf_forecast;

  if (attacker_strength_for_forecast >= 0.0) {
    out.target.has_attacker_strength_forecast = true;
    out.target.attacker_strength_test = std::max(0.0, attacker_strength_for_forecast);
    out.target.forecast_at_attacker_strength = forecast_for(attacker_strength_for_forecast, forts_effective, defender_arty);
  }

  // --- Staging colony suggestions ---
  const double take_frac_raw = (opt.max_take_fraction_of_surplus >= 0.0)
                                  ? opt.max_take_fraction_of_surplus
                                  : sim.cfg().auto_troop_max_take_fraction_of_surplus;
  const double take_frac = std::clamp(take_frac_raw, 0.0, 1.0);

  const bool have_speed = (opt.planning_speed_km_s > 1e-9);
  const bool have_start = (opt.start_system_id != kInvalidId);

  std::vector<InvasionStagingOption> options;
  options.reserve(s.colonies.size());

  for (Id cid : sorted_keys(s.colonies)) {
    const Colony* c = find_ptr(s.colonies, cid);
    if (!c) continue;
    if (opt.attacker_faction_id != kInvalidId && c->faction_id != opt.attacker_faction_id) continue;

    const Body* b = find_ptr(s.bodies, c->body_id);
    if (!b || b->system_id == kInvalidId) continue;

    if (opt.attacker_faction_id != kInvalidId && opt.restrict_to_discovered) {
      if (!sim.is_system_discovered_by_faction(opt.attacker_faction_id, b->system_id)) continue;
    }

    // Use battle defender strength when the staging colony is actively under siege.
    double gf = std::max(0.0, c->ground_forces);
    if (auto itb = s.ground_battles.find(c->id); itb != s.ground_battles.end()) {
      const auto& gb = itb->second;
      if (gb.defender_faction_id == c->faction_id) {
        gf = std::max(0.0, gb.defender_strength);
      }
    }

    const double desired = std::max(0.0, c->garrison_target_strength);
    const double surplus = std::max(0.0, gf - desired);
    if (surplus <= 1e-6) continue;

    InvasionStagingOption o;
    o.colony_id = c->id;
    o.surplus_strength = surplus;
    o.take_cap_strength = surplus * take_frac;
    if (o.take_cap_strength <= 1e-6) continue;

    if (have_speed) {
      const double eta_stage_to_target = estimate_eta_days_to_pos(sim, b->system_id, b->position_mkm,
                                                                 opt.attacker_faction_id, opt.planning_speed_km_s,
                                                                 tgt_body->system_id, tgt_body->position_mkm,
                                                                 opt.restrict_to_discovered);
      o.eta_stage_to_target_days = eta_stage_to_target;

      if (have_start) {
        const double eta_start_to_stage = estimate_eta_days_to_pos(sim, opt.start_system_id, opt.start_pos_mkm,
                                                                   opt.attacker_faction_id, opt.planning_speed_km_s,
                                                                   b->system_id, b->position_mkm,
                                                                   opt.restrict_to_discovered);
        o.eta_start_to_stage_days = eta_start_to_stage;
      } else {
        o.eta_start_to_stage_days = 0.0;
      }

      o.eta_total_days = o.eta_start_to_stage_days + o.eta_stage_to_target_days;

      if (!finite_pos(o.eta_stage_to_target_days) || !finite_pos(o.eta_start_to_stage_days)) {
        continue;
      }

      // Score similar to the AI: strongly prefer large surpluses, but penalize distance.
      o.score = o.take_cap_strength * 1000.0 - o.eta_total_days * 10.0;
    } else {
      o.eta_start_to_stage_days = 0.0;
      o.eta_stage_to_target_days = 0.0;
      o.eta_total_days = 0.0;
      o.score = o.take_cap_strength;
    }

    options.push_back(o);
  }

  std::sort(options.begin(), options.end(), [&](const InvasionStagingOption& a, const InvasionStagingOption& b) {
    if (std::abs(a.score - b.score) > 1e-9) return a.score > b.score;
    return a.colony_id < b.colony_id;
  });

  const int max_opts = std::max(0, opt.max_staging_options);
  if (max_opts > 0 && static_cast<int>(options.size()) > max_opts) {
    options.resize(static_cast<std::size_t>(max_opts));
  }

  out.staging_options = std::move(options);

  out.ok = true;
  out.message.clear();
  return out;
}

}  // namespace nebula4x
