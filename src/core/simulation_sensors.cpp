#include "simulation_sensors.h"

#include "nebula4x/core/simulation.h"

#include "simulation_internal.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace nebula4x::sim_sensors {
namespace {

inline double sane_multiplier(double x, double fallback) {
  if (!std::isfinite(x)) return fallback;
  if (x < 0.0) return 0.0;
  // Avoid absurdly large values from malformed configs.
  return std::min(x, 100.0);
}

inline double mode_range_multiplier(const Simulation& sim, SensorMode mode) {
  const auto& cfg = sim.cfg();
  switch (mode) {
    case SensorMode::Passive: return sane_multiplier(cfg.sensor_mode_passive_range_multiplier, 1.0);
    case SensorMode::Active: return sane_multiplier(cfg.sensor_mode_active_range_multiplier, 1.0);
    case SensorMode::Normal: default: return 1.0;
  }
}

inline double mode_signature_multiplier(const Simulation& sim, SensorMode mode) {
  const auto& cfg = sim.cfg();
  switch (mode) {
    case SensorMode::Passive: return sane_multiplier(cfg.sensor_mode_passive_signature_multiplier, 1.0);
    case SensorMode::Active: return sane_multiplier(cfg.sensor_mode_active_signature_multiplier, 1.0);
    case SensorMode::Normal: default: return 1.0;
  }
}

} // namespace

double max_signature_multiplier_for_detection(const Simulation& sim) {
  // Base design signature is validated as <= 1.0 (stealth only reduces).
  //
  // Mechanics that can increase detectability above 1.0:
  //  - SensorMode::Active (EM signature)
  //  - Optional thermal bloom from ship heat (if enabled)
  const double em = mode_signature_multiplier(sim, SensorMode::Active);

  double heat_max = 1.0;
  if (sim.cfg().enable_ship_heat) {
    const double per = sim.cfg().ship_heat_signature_multiplier_per_fraction;
    if (std::isfinite(per) && per > 0.0) {
      heat_max = sane_multiplier(sim.cfg().ship_heat_signature_multiplier_max, 1.0);
      heat_max = std::max(1.0, heat_max);
    }
  }

  const double max_sig = em * heat_max;
  return std::clamp(max_sig, 1.0, 100.0);
}

double effective_signature_multiplier(const Simulation& sim, const Ship& ship, const ShipDesign* design) {
  const ShipDesign* d = design ? design : sim.find_design(ship.design_id);

  double base = d ? d->signature_multiplier : 1.0;
  if (!std::isfinite(base)) base = 1.0;
  base = std::clamp(base, 0.0, 1.0);

  // EMCON (sensor mode) only applies to ships with sensors.
  double emcon = 1.0;
  if (d && d->sensor_range_mkm > 0.0) {
    // If sensors are explicitly disabled, treat as "passive" for detectability.
    const SensorMode mode = ship.power_policy.sensors_enabled ? ship.sensor_mode : SensorMode::Passive;
    emcon = mode_signature_multiplier(sim, mode);
  }

  // Optional thermal bloom from ship heat.
  const double heat_mult = sim.ship_heat_signature_multiplier(ship);

  double eff = base * emcon * heat_mult;
  if (!std::isfinite(eff)) eff = base;

  // Environmental hiding: targets inside dense nebulas should be somewhat
  // harder to detect, not just sensor sources. We approximate line-of-sight
  // attenuation by lerping the target's signature multiplier toward the
  // local sensor environment multiplier.
  const double w = std::clamp(sim.cfg().nebula_target_signature_env_weight, 0.0, 1.0);
  if (w > 1e-9) {
    const double env = sim.system_sensor_environment_multiplier_at(ship.system_id, ship.position_mkm);
    const double env01 = std::clamp(env, 0.0, 1.0);
    const double m = (1.0 - w) + w * env01;
    eff *= std::clamp(m, 0.05, 1.0);
  }

  const double max_sig = max_signature_multiplier_for_detection(sim);
  eff = std::clamp(eff, 0.0, max_sig);
  return eff;
}

double sensor_range_mkm_with_mode(const Simulation& sim, const Ship& ship, const ShipDesign& design) {
  double range = std::max(0.0, design.sensor_range_mkm);
  if (range <= 0.0) return 0.0;

  // Respect power policies / power availability.
  const auto p = sim_internal::compute_power_allocation(design, ship.power_policy);
  if (!p.sensors_online) return 0.0;

  range *= mode_range_multiplier(sim, ship.sensor_mode);
  range *= sim.ship_heat_sensor_range_multiplier(ship);
  range *= sim.ship_subsystem_sensor_range_multiplier(ship);
  if (!std::isfinite(range) || range < 0.0) return 0.0;
  return range;
}

std::vector<SensorSource> gather_sensor_sources(const Simulation& sim, Id faction_id, Id system_id) {
  std::vector<SensorSource> out;

  const auto& s = sim.state();
  const auto* sys = find_ptr(s.systems, system_id);
  if (!sys) return out;

  // Environmental sensor attenuation (nebula + storms).
  //
  // With nebula microfields enabled, attenuation becomes position-dependent.
  // We compute per-source multipliers so ships can find/hold "clear lanes" and
  // dense hiding pockets in a single system.

  // Mutual-friendly factions share sensor coverage.
  std::vector<Id> sensor_factions;
  sensor_factions.reserve(s.factions.size());
  sensor_factions.push_back(faction_id);
  for (const auto& [other_id, _] : s.factions) {
    if (other_id == faction_id) continue;
    if (sim.are_factions_mutual_friendly(faction_id, other_id)) sensor_factions.push_back(other_id);
  }
  std::sort(sensor_factions.begin(), sensor_factions.end());
  sensor_factions.erase(std::unique(sensor_factions.begin(), sensor_factions.end()), sensor_factions.end());

  auto is_sensor_faction = [&](Id fid) -> bool {
    return std::binary_search(sensor_factions.begin(), sensor_factions.end(), fid);
  };

  // --- friendly ship sensors ---
  for (Id ship_id : sys->ships) {
    const auto* sh = find_ptr(s.ships, ship_id);
    if (!sh) continue;
    if (!is_sensor_faction(sh->faction_id)) continue;

    const auto* d = sim.find_design(sh->design_id);
    if (!d) continue;

    const double env_mult = sim.system_sensor_environment_multiplier_at(system_id, sh->position_mkm);
    const double range_mkm = sensor_range_mkm_with_mode(sim, *sh, *d) * env_mult;
    if (range_mkm <= 0.0) continue;

    double eccm = 0.0;
    if (d) eccm = std::max(0.0, d->eccm_strength);
    out.push_back(SensorSource{sh->position_mkm, range_mkm, ship_id, eccm});
  }

  // --- colony-based sensors (installations) ---
  // For now, treat the best sensor range among a colony's installations as the
  // colony's sensor coverage. (Multiple sensor installations don't stack range.)
  for (const auto& [col_id, col] : s.colonies) {
    (void)col_id;
    if (!is_sensor_faction(col.faction_id)) continue;

    const auto* body = find_ptr(s.bodies, col.body_id);
    if (!body) continue;
    if (body->system_id != system_id) continue;

    double best_mkm = 0.0;
    for (const auto& [inst_id, count] : col.installations) {
      if (count <= 0) continue;
      const auto it = sim.content().installations.find(inst_id);
      if (it == sim.content().installations.end()) continue;
      best_mkm = std::max(best_mkm, std::max(0.0, it->second.sensor_range_mkm));
    }

    const double env_mult = sim.system_sensor_environment_multiplier_at(system_id, body->position_mkm);
    best_mkm *= env_mult;
    if (best_mkm <= 0.0) continue;
    out.push_back(SensorSource{body->position_mkm, best_mkm, kInvalidId, 0.0});
  }

  return out;
}

bool any_source_detects(const std::vector<SensorSource>& sources, const Vec2& target_pos_mkm,
                        double target_signature_multiplier, double target_ecm_strength) {
  // Signature scales detection range:
  //  - < 1.0 => harder to detect (stealthy)
  //  - = 1.0 => baseline
  //  - > 1.0 => easier to detect (high emissions / active sensors)
  double sig = std::isfinite(target_signature_multiplier) ? target_signature_multiplier : 1.0;
  if (sig < 0.0) sig = 0.0;

  for (const auto& src : sources) {
    if (src.range_mkm <= 0.0) continue;

    // Electronic warfare: ECCM extends effective detection; ECM reduces it.
    double ecm = std::isfinite(target_ecm_strength) ? target_ecm_strength : 0.0;
    if (ecm < 0.0) ecm = 0.0;

    double eccm = std::isfinite(src.eccm_strength) ? src.eccm_strength : 0.0;
    if (eccm < 0.0) eccm = 0.0;

    // (1 + eccm) / (1 + ecm) multiplier, clamped to keep the game stable.
    double ew_mult = (1.0 + eccm) / (1.0 + ecm);
    if (!std::isfinite(ew_mult)) ew_mult = 1.0;
    ew_mult = std::clamp(ew_mult, 0.1, 10.0);

    const double r = src.range_mkm * sig * ew_mult;
    if (r <= 0.0) continue;

    if ((target_pos_mkm - src.pos_mkm).length() <= r) return true;
  }
  return false;
}

} // namespace nebula4x::sim_sensors
