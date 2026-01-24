#include "nebula4x/core/simulation.h"

#include "nebula4x/util/trace_events.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>

namespace nebula4x {

namespace {

double step_toward(double cur, double target, double max_step) {
  if (max_step <= 0.0) return cur;
  const double delta = target - cur;
  if (std::abs(delta) <= max_step) return target;
  return cur + (delta > 0.0 ? max_step : -max_step);
}

} // namespace

void Simulation::tick_terraforming(double dt_days) {
  if (dt_days <= 0.0) return;
  NEBULA4X_TRACE_SCOPE("tick_terraforming", "sim.terraform");
  const double dT_per_pt_base = std::max(0.0, cfg_.terraforming_temp_k_per_point_day);
  const double dA_per_pt_base = std::max(0.0, cfg_.terraforming_atm_per_point_day);
  const double tolT = std::max(0.0, cfg_.terraforming_temp_tolerance_k);
  const double tolA = std::max(0.0, cfg_.terraforming_atm_tolerance);

  const double cost_d = std::max(0.0, cfg_.terraforming_duranium_per_point);
  const double cost_n = std::max(0.0, cfg_.terraforming_neutronium_per_point);

  const bool split_axes = cfg_.terraforming_split_points_between_axes;
  const bool scale_mass = cfg_.terraforming_scale_with_body_mass;
  const double min_mass = std::max(1e-6, cfg_.terraforming_min_mass_earths);
  const double mass_exp = std::max(0.0, cfg_.terraforming_mass_scaling_exponent);

  // Aggregate *affordable* terraforming points per body from colonies.
  // (Multiple colonies on the same body is unusual, but supported.)
  //
  // Optional mineral costs are consumed at the colony level so that logistics
  // constraints naturally throttle terraforming output.
  std::unordered_map<Id, double> points_by_body;
  points_by_body.reserve(state_.colonies.size());

  for (auto& [cid, col] : state_.colonies) {
    const Body* body = find_ptr(state_.bodies, col.body_id);
    if (!body) continue;
    const bool has_target = (body->terraforming_target_temp_k > 0.0 || body->terraforming_target_atm > 0.0);
    if (!has_target || body->terraforming_complete) continue;

    const double pts_per_day = std::max(0.0, terraforming_points_per_day(col));
    if (pts_per_day <= 1e-9) continue;

    double pts = pts_per_day * dt_days;
    if (pts <= 1e-9) continue;

    // Mineral affordability scaling (mirrors troop training).
    double afford = 1.0;
    if (cost_d > 1e-12) {
      const double need = pts * cost_d;
      const double have = col.minerals.contains("Duranium") ? std::max(0.0, col.minerals["Duranium"]) : 0.0;
      if (need > 1e-12) afford = std::min(afford, have / need);
    }
    if (cost_n > 1e-12) {
      const double need = pts * cost_n;
      const double have = col.minerals.contains("Neutronium") ? std::max(0.0, col.minerals["Neutronium"]) : 0.0;
      if (need > 1e-12) afford = std::min(afford, have / need);
    }
    afford = std::clamp(afford, 0.0, 1.0);
    pts *= afford;
    if (pts <= 1e-9) continue;

    if (cost_d > 1e-12) {
      col.minerals["Duranium"] = std::max(0.0, col.minerals["Duranium"] - pts * cost_d);
    }
    if (cost_n > 1e-12) {
      col.minerals["Neutronium"] = std::max(0.0, col.minerals["Neutronium"] - pts * cost_n);
    }

    points_by_body[col.body_id] += pts;
  }

  for (auto& [bid, body] : state_.bodies) {
    const bool has_target_T = (body.terraforming_target_temp_k > 0.0);
    const bool has_target_A = (body.terraforming_target_atm > 0.0);
    if (!has_target_T && !has_target_A) continue;
    if (body.terraforming_complete) continue;

    const auto itp = points_by_body.find(bid);
    const double pts_total = (itp == points_by_body.end()) ? 0.0 : itp->second;
    if (pts_total <= 1e-9) continue;

    // Optional scaling: smaller bodies are easier to terraform.
    double dT_per_pt = dT_per_pt_base;
    double dA_per_pt = dA_per_pt_base;
    if (scale_mass && (dT_per_pt > 1e-12 || dA_per_pt > 1e-12)) {
      double mass = body.mass_earths;
      if (!(mass > 0.0) || !std::isfinite(mass)) mass = 1.0;
      mass = std::max(min_mass, mass);
      const double scale = 1.0 / std::pow(mass, mass_exp);
      dT_per_pt *= scale;
      dA_per_pt *= scale;
    }

    // Initialize unknown environment to a plausible baseline if needed.
    if (has_target_T && body.surface_temp_k <= 0.0) body.surface_temp_k = body.terraforming_target_temp_k;
    if (has_target_A && body.atmosphere_atm <= 0.0) body.atmosphere_atm = 0.0;

    // Split points between temperature and atmosphere when both are active.
    //
    // In the legacy model, a single point implicitly advanced both axes at
    // full strength, creating an accidental "double benefit". When enabled,
    // we treat points as a shared budget that must be allocated.
    double pts_T = pts_total;
    double pts_A = pts_total;
    if (split_axes) {
      const double deltaT = has_target_T ? std::abs(body.surface_temp_k - body.terraforming_target_temp_k) : 0.0;
      const double deltaA = has_target_A ? std::abs(body.atmosphere_atm - body.terraforming_target_atm) : 0.0;

      const bool need_T = has_target_T && dT_per_pt > 1e-12 && deltaT > tolT + 1e-12;
      const bool need_A = has_target_A && dA_per_pt > 1e-12 && deltaA > tolA + 1e-12;

      if (need_T && need_A) {
        const double wT = deltaT / dT_per_pt;
        const double wA = deltaA / dA_per_pt;
        const double sum = wT + wA;
        const double fracT = (sum > 1e-12) ? std::clamp(wT / sum, 0.0, 1.0) : 0.5;
        pts_T = pts_total * fracT;
        pts_A = pts_total - pts_T;
      } else {
        pts_T = need_T ? pts_total : 0.0;
        pts_A = need_A ? pts_total : 0.0;
      }
    }

    if (has_target_T && dT_per_pt > 1e-12 && pts_T > 1e-12) {
      body.surface_temp_k = step_toward(body.surface_temp_k, body.terraforming_target_temp_k, pts_T * dT_per_pt);
    }
    if (has_target_A && dA_per_pt > 1e-12 && pts_A > 1e-12) {
      body.atmosphere_atm = step_toward(body.atmosphere_atm, body.terraforming_target_atm, pts_A * dA_per_pt);
      if (body.atmosphere_atm < 0.0) body.atmosphere_atm = 0.0;
    }

    const bool done_T = !has_target_T || (std::abs(body.surface_temp_k - body.terraforming_target_temp_k) <= tolT);
    const bool done_A = !has_target_A || (std::abs(body.atmosphere_atm - body.terraforming_target_atm) <= tolA);
    if (done_T && done_A) {
      body.terraforming_complete = true;

      // Find a colony on this body to attach context (for UI navigation).
      Id ctx_colony = kInvalidId;
      for (const auto& [cid, col] : state_.colonies) {
        if (col.body_id == bid) {
          ctx_colony = cid;
          break;
        }
      }

      EventContext ctx;
      ctx.system_id = body.system_id;
      ctx.colony_id = ctx_colony;
      if (ctx_colony != kInvalidId) {
        if (const auto* col = find_ptr(state_.colonies, ctx_colony)) {
          ctx.faction_id = col->faction_id;
        }
      }

      push_event(EventLevel::Info, EventCategory::Terraforming,
                 "Terraforming complete on " + body.name,
                 ctx);
    }
  }
}

} // namespace nebula4x
