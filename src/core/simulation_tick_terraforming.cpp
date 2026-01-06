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
  // Aggregate terraforming points per body from colonies.
  // (Multiple colonies on the same body is unusual, but supported.)
  std::unordered_map<Id, double> points_by_body;
  points_by_body.reserve(state_.colonies.size());
  for (const auto& [_, col] : state_.colonies) {
    const double pts = std::max(0.0, terraforming_points_per_day(col));
    if (pts <= 1e-9) continue;
    points_by_body[col.body_id] += pts * dt_days;
  }

  const double dT_per_pt = std::max(0.0, cfg_.terraforming_temp_k_per_point_day);
  const double dA_per_pt = std::max(0.0, cfg_.terraforming_atm_per_point_day);
  const double tolT = std::max(0.0, cfg_.terraforming_temp_tolerance_k);
  const double tolA = std::max(0.0, cfg_.terraforming_atm_tolerance);

  for (auto& [bid, body] : state_.bodies) {
    const bool has_target_T = (body.terraforming_target_temp_k > 0.0);
    const bool has_target_A = (body.terraforming_target_atm > 0.0);
    if (!has_target_T && !has_target_A) continue;
    if (body.terraforming_complete) continue;

    const auto itp = points_by_body.find(bid);
    const double pts = (itp == points_by_body.end()) ? 0.0 : itp->second;
    if (pts <= 1e-9) continue;

    // Initialize unknown environment to a plausible baseline if needed.
    if (has_target_T && body.surface_temp_k <= 0.0) body.surface_temp_k = body.terraforming_target_temp_k;
    if (has_target_A && body.atmosphere_atm <= 0.0) body.atmosphere_atm = 0.0;

    if (has_target_T) {
      body.surface_temp_k = step_toward(body.surface_temp_k, body.terraforming_target_temp_k, pts * dT_per_pt);
    }
    if (has_target_A) {
      body.atmosphere_atm = step_toward(body.atmosphere_atm, body.terraforming_target_atm, pts * dA_per_pt);
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

      push_event(EventLevel::Info, EventCategory::General,
                 "Terraforming complete on " + body.name,
                 ctx);
    }
  }
}

} // namespace nebula4x
