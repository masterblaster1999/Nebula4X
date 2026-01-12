#include "nebula4x/core/simulation.h"

#include "simulation_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>

#include "nebula4x/util/log.h"
#include "nebula4x/util/trace_events.h"

namespace nebula4x {
namespace {
using sim_internal::sorted_keys;
using sim_internal::compute_power_allocation;

static double clamp01(double v) {
  return std::clamp(v, 0.0, 1.0);
}

static double safe_non_negative(double v) {
  if (!std::isfinite(v) || v < 0.0) return 0.0;
  return v;
}

static double linear_penalty_multiplier(double heat_frac, double start_frac, double full_frac,
                                       double min_multiplier) {
  if (!std::isfinite(heat_frac)) return 1.0;

  start_frac = safe_non_negative(start_frac);
  full_frac = safe_non_negative(full_frac);

  if (min_multiplier >= 1.0) return 1.0;
  min_multiplier = std::clamp(min_multiplier, 0.0, 1.0);

  if (heat_frac <= start_frac + 1e-12) return 1.0;

  // Degenerate case: treat as a step function.
  if (full_frac <= start_frac + 1e-9) {
    return min_multiplier;
  }

  if (heat_frac >= full_frac - 1e-12) return min_multiplier;

  const double t = clamp01((heat_frac - start_frac) / (full_frac - start_frac));
  return 1.0 + (min_multiplier - 1.0) * t;
}

static bool is_player_faction(const GameState& s, Id faction_id) {
  const auto* fac = find_ptr(s.factions, faction_id);
  return fac && fac->control == FactionControl::Player;
}

} // namespace


double Simulation::ship_heat_fraction(const Ship& ship) const {
  if (!cfg_.enable_ship_heat) return 0.0;

  const auto* d = find_design(ship.design_id);
  if (!d) return 0.0;

  const double cap = safe_non_negative(cfg_.ship_heat_base_capacity_per_mass_ton) * safe_non_negative(d->mass_tons) +
                     safe_non_negative(d->heat_capacity_bonus);
  if (!(cap > 1e-9)) return 0.0;

  const double heat = safe_non_negative(ship.heat);
  // Allow >1.0 for overheating; keep it bounded for safety.
  return std::clamp(heat / cap, 0.0, 10.0);
}

double Simulation::ship_heat_speed_multiplier(const Ship& ship) const {
  if (!cfg_.enable_ship_heat) return 1.0;
  const double frac = ship_heat_fraction(ship);
  return linear_penalty_multiplier(frac, cfg_.ship_heat_penalty_start_fraction, cfg_.ship_heat_penalty_full_fraction,
                                   cfg_.ship_heat_min_speed_multiplier);
}

double Simulation::ship_heat_sensor_range_multiplier(const Ship& ship) const {
  if (!cfg_.enable_ship_heat) return 1.0;
  const double frac = ship_heat_fraction(ship);
  return linear_penalty_multiplier(frac, cfg_.ship_heat_penalty_start_fraction, cfg_.ship_heat_penalty_full_fraction,
                                   cfg_.ship_heat_min_sensor_range_multiplier);
}

double Simulation::ship_heat_weapon_output_multiplier(const Ship& ship) const {
  if (!cfg_.enable_ship_heat) return 1.0;
  const double frac = ship_heat_fraction(ship);
  return linear_penalty_multiplier(frac, cfg_.ship_heat_penalty_start_fraction, cfg_.ship_heat_penalty_full_fraction,
                                   cfg_.ship_heat_min_weapon_output_multiplier);
}

double Simulation::ship_heat_shield_regen_multiplier(const Ship& ship) const {
  if (!cfg_.enable_ship_heat) return 1.0;
  const double frac = ship_heat_fraction(ship);
  return linear_penalty_multiplier(frac, cfg_.ship_heat_penalty_start_fraction, cfg_.ship_heat_penalty_full_fraction,
                                   cfg_.ship_heat_min_shield_regen_multiplier);
}


double Simulation::ship_heat_signature_multiplier(const Ship& ship) const {
  if (!cfg_.enable_ship_heat) return 1.0;

  const double per = cfg_.ship_heat_signature_multiplier_per_fraction;
  if (!std::isfinite(per) || per <= 0.0) return 1.0;

  const double max_mult = std::max(1.0, std::isfinite(cfg_.ship_heat_signature_multiplier_max)
                                         ? cfg_.ship_heat_signature_multiplier_max
                                         : 1.0);

  const double frac = ship_heat_fraction(ship);
  if (!(frac > 1e-9)) return 1.0;

  const double raw = 1.0 + per * frac;
  if (!std::isfinite(raw)) return max_mult;
  return std::clamp(raw, 1.0, max_mult);
}


void Simulation::tick_heat(double dt_days) {
  NEBULA4X_TRACE_SCOPE("tick_heat", "sim.heat");
  if (!cfg_.enable_ship_heat) return;

  dt_days = std::clamp(dt_days, 0.0, 10.0);
  if (dt_days <= 0.0) return;

  const auto ship_ids = sorted_keys(state_.ships);
  for (Id sid : ship_ids) {
    auto* sh = find_ptr(state_.ships, sid);
    if (!sh) continue;
    if (sh->hp <= 0.0) continue;

    const auto* d = find_design(sh->design_id);
    if (!d) {
      // Design missing -> avoid NaNs.
      sh->heat = 0.0;
      sh->heat_state = 0;
      continue;
    }

    const double mass_tons = safe_non_negative(d->mass_tons);
    const double cap = safe_non_negative(cfg_.ship_heat_base_capacity_per_mass_ton) * mass_tons +
                       safe_non_negative(d->heat_capacity_bonus);
    if (!(cap > 1e-9)) {
      // No capacity -> keep heat at 0.
      sh->heat = 0.0;
      sh->heat_state = 0;
      continue;
    }

    // Estimate online power use for this tick (respects power policy).
    const auto p = compute_power_allocation(*d, sh->power_policy);
    const double other_use = std::max(
        0.0, safe_non_negative(d->power_use_total) -
                 (safe_non_negative(d->power_use_engines) + safe_non_negative(d->power_use_sensors) +
                  safe_non_negative(d->power_use_weapons) + safe_non_negative(d->power_use_shields)));

    double online_power_use = other_use;
    if (p.engines_online) online_power_use += safe_non_negative(d->power_use_engines);
    if (p.sensors_online) online_power_use += safe_non_negative(d->power_use_sensors);
    if (p.weapons_online) online_power_use += safe_non_negative(d->power_use_weapons);
    if (p.shields_online) online_power_use += safe_non_negative(d->power_use_shields);

    const double gen_per_day = safe_non_negative(cfg_.ship_heat_generation_per_power_use_per_day) * online_power_use +
                               safe_non_negative(d->heat_generation_bonus_per_day);

    const double diss_per_day =
        safe_non_negative(cfg_.ship_heat_base_dissipation_per_mass_ton_per_day) * mass_tons +
        safe_non_negative(d->heat_dissipation_bonus_per_day);

    const double delta = (gen_per_day - diss_per_day) * dt_days;

    sh->heat = safe_non_negative(sh->heat + delta);

    const double frac = std::clamp(sh->heat / cap, 0.0, 10.0);

    // Bucket for event throttling.
    std::uint8_t new_state = 0;
    if (frac >= cfg_.ship_heat_damage_threshold_fraction) {
      new_state = 3;
    } else if (frac >= cfg_.ship_heat_penalty_full_fraction) {
      new_state = 2;
    } else if (frac >= cfg_.ship_heat_penalty_start_fraction) {
      new_state = 1;
    }

    const std::uint8_t old_state = sh->heat_state;
    sh->heat_state = new_state;

    // Emit warnings for player-controlled factions on upward transitions.
    if (new_state > old_state && is_player_faction(state_, sh->faction_id)) {
      EventContext ctx;
      ctx.faction_id = sh->faction_id;
      ctx.system_id = sh->system_id;
      ctx.ship_id = sh->id;

      const int pct = static_cast<int>(std::lround(frac * 100.0));

      if (new_state == 1) {
        push_event(EventLevel::Info, EventCategory::General,
                   "Ship heat rising: " + sh->name + " (" + std::to_string(pct) + "% of capacity)", ctx);
      } else if (new_state == 2) {
        push_event(EventLevel::Warn, EventCategory::General,
                   "Ship overheating: " + sh->name + " (" + std::to_string(pct) +
                       "% of capacity, performance reduced)",
                   ctx);
      } else if (new_state == 3) {
        push_event(EventLevel::Warn, EventCategory::General,
                   "Ship critical overheating: " + sh->name + " (" + std::to_string(pct) +
                       "% of capacity, hull taking damage)",
                   ctx);
      }
    }

    // Apply severe overheating hull damage.
    if (frac >= cfg_.ship_heat_damage_threshold_fraction) {
      const double thresh = std::max(0.0, cfg_.ship_heat_damage_threshold_fraction);
      const double denom = std::max(1e-6, 2.0 - thresh);
      double t = (frac - thresh) / denom;
      // Allow escalating damage beyond 200% but keep it bounded.
      t = std::clamp(t, 0.0, 3.0);

      const double max_hp = std::max(1e-6, safe_non_negative(d->max_hp));
      const double dmg_per_day_at_200 = safe_non_negative(cfg_.ship_heat_damage_fraction_per_day_at_200pct) * max_hp;
      const double dmg = dmg_per_day_at_200 * t * dt_days;

      if (dmg > 1e-9) {
        sh->hp = std::max(0.0, sh->hp - dmg);
      }

      // If the ship dies from heat, destroy it immediately.
      if (sh->hp <= 0.0) {
        const Ship victim = *sh; // copy for message/salvage

        // Spawn a salvageable wreck (same coarse model as combat destruction).
        if (cfg_.enable_wrecks) {
          std::unordered_map<std::string, double> salvage;

          const double cargo_frac = std::clamp(cfg_.wreck_cargo_salvage_fraction, 0.0, 1.0);
          if (cargo_frac > 1e-9) {
            for (const auto& [mineral, tons] : victim.cargo) {
              if (tons > 1e-9) salvage[mineral] += tons * cargo_frac;
            }
          }

          const double hull_frac = std::max(0.0, cfg_.wreck_hull_salvage_fraction);
          if (hull_frac > 1e-9) {
            const double hull_tons = std::max(0.0, mass_tons) * hull_frac;

            // Prefer an explicit shipyard mineral recipe if available.
            const InstallationDef* yard = nullptr;
            if (auto it_y = content_.installations.find("shipyard"); it_y != content_.installations.end()) {
              yard = &it_y->second;
            }

            if (yard && !yard->build_costs_per_ton.empty()) {
              for (const auto& [mineral, cost_per_ton] : yard->build_costs_per_ton) {
                if (cost_per_ton > 1e-12) salvage[mineral] += hull_tons * cost_per_ton;
              }
            } else {
              salvage["Duranium"] += hull_tons * 1.0;
              salvage["Neutronium"] += hull_tons * 0.1;
            }
          }

          // Prune non-positive / non-finite entries.
          for (auto it_salv = salvage.begin(); it_salv != salvage.end();) {
            const double v = it_salv->second;
            if (!(v > 1e-9) || std::isnan(v) || std::isinf(v)) {
              it_salv = salvage.erase(it_salv);
            } else {
              ++it_salv;
            }
          }

          if (!salvage.empty()) {
            Wreck w;
            w.id = allocate_id(state_);
            w.name = "Wreck: " + victim.name;
            w.system_id = victim.system_id;
            w.position_mkm = victim.position_mkm;
            w.minerals = std::move(salvage);
            w.source_ship_id = victim.id;
            w.source_faction_id = victim.faction_id;
            w.source_design_id = victim.design_id;
            w.created_day = state_.date.days_since_epoch();
            state_.wrecks[w.id] = std::move(w);
          }
        }

        // Remove the ship from the simulation state.
        const Id sys_id = victim.system_id;
        if (auto* sys = find_ptr(state_.systems, sys_id)) {
          sys->ships.erase(std::remove(sys->ships.begin(), sys->ships.end(), victim.id), sys->ships.end());
        }

        state_.ship_orders.erase(victim.id);
        state_.ships.erase(victim.id);
        remove_ship_from_fleets(victim.id);
        for (auto& [_, fac] : state_.factions) {
          fac.ship_contacts.erase(victim.id);
        }

        if (is_player_faction(state_, victim.faction_id)) {
          EventContext ctx;
          ctx.faction_id = victim.faction_id;
          ctx.system_id = victim.system_id;
          ctx.ship_id = victim.id;
          const std::string msg = "Ship destroyed by overheating: " + victim.name;
          nebula4x::log::warn(msg);
          push_event(EventLevel::Warn, EventCategory::General, msg, ctx);
        }
      }
    }
  }
}

} // namespace nebula4x
