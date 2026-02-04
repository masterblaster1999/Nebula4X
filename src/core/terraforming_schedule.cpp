#include "nebula4x/core/terraforming_schedule.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "nebula4x/core/game_state.h"
#include "nebula4x/core/simulation.h"

namespace nebula4x {
namespace {

double clamp01(double v) {
  if (!std::isfinite(v)) return 0.0;
  if (v < 0.0) return 0.0;
  if (v > 1.0) return 1.0;
  return v;
}

// Move x towards target by at most step (step >= 0).
double step_toward(double x, double target, double step) {
  if (!std::isfinite(x) || !std::isfinite(target) || !std::isfinite(step)) return x;
  if (step < 0.0) step = 0.0;
  if (x < target) return std::min(target, x + step);
  if (x > target) return std::max(target, x - step);
  return x;
}

bool approx_equal(double a, double b, double tol) {
  if (!std::isfinite(a) || !std::isfinite(b) || !std::isfinite(tol)) return false;
  return std::abs(a - b) <= std::max(0.0, tol);
}

struct ColonyBudget {
  Id colony_id{kInvalidId};
  double pts_per_day{0.0};
  double dur_rem{0.0};
  double neu_rem{0.0};
};

} // namespace

TerraformingSchedule estimate_terraforming_schedule(const Simulation& sim, Id body_id,
                                                    const TerraformingScheduleOptions& opt) {
  TerraformingSchedule out;
  out.ok = false;
  out.body_id = body_id;

  const auto& st = sim.state();
  const auto* body = find_ptr(st.bodies, body_id);
  if (!body) return out;

  out.system_id = body->system_id;

  const bool has_target = (body->terraforming_target_temp_k > 0.0 || body->terraforming_target_atm > 0.0 ||
                           body->terraforming_target_o2_atm > 0.0);
  out.has_target = has_target;
  out.target_temp_k = body->terraforming_target_temp_k;
  out.target_atm = body->terraforming_target_atm;
  out.target_o2_atm = body->terraforming_target_o2_atm;

  // If there is no target, the forecast is still "ok" but has nothing to do.
  if (!has_target) {
    out.ok = true;
    out.start_temp_k = body->surface_temp_k;
    out.start_atm = body->atmosphere_atm;
    out.start_o2_atm = body->oxygen_atm;
    out.end_temp_k = out.start_temp_k;
    out.end_atm = out.start_atm;
    out.end_o2_atm = out.start_o2_atm;
    out.complete = false;
    out.days_to_complete = 0;
    return out;
  }

  // Mirror the simulation tick's initialization behavior: if temp is <= 0
  // (i.e. unknown/uninitialized), treat it as already at the target.
  double temp = body->surface_temp_k;
  if (!(temp > 0.0) && out.target_temp_k > 0.0) temp = out.target_temp_k;

  // Atmosphere: if uninitialized, start at 0.
  double atm = body->atmosphere_atm;
  if (!(atm > 0.0) && out.target_atm > 0.0) atm = 0.0;

  // Oxygen: if uninitialized, start at 0.
  double o2 = body->oxygen_atm;
  if (!(o2 > 0.0) && out.target_o2_atm > 0.0) o2 = 0.0;

  out.start_temp_k = temp;
  out.start_atm = atm;
  out.start_o2_atm = o2;

  // Config knobs.
  const auto& cfg = sim.cfg();

  double dT_per_pt = std::max(0.0, cfg.terraforming_temp_k_per_point_day);
  double dA_per_pt = std::max(0.0, cfg.terraforming_atm_per_point_day);
  double dO_per_pt = std::max(0.0, cfg.terraforming_o2_atm_per_point_day);

  const double tolT = std::max(0.0, cfg.terraforming_temp_tolerance_k);
  const double tolA = std::max(0.0, cfg.terraforming_atm_tolerance);
  const double tolO = std::max(0.0, cfg.terraforming_o2_tolerance_atm);
  const double max_o2_frac = std::clamp(cfg.terraforming_o2_max_fraction_of_atm, 0.0, 1.0);

  // Mass scaling: smaller bodies terraform faster.
  if (cfg.terraforming_scale_with_body_mass) {
    const double mass = std::max(0.0, body->mass_earths);
    const double min_mass = std::max(1e-6, cfg.terraforming_min_mass_earths);
    const double exp = cfg.terraforming_mass_scaling_exponent;
    const double scaled = std::pow(std::max(min_mass, mass), exp);
    if (scaled > 1e-12) {
      dT_per_pt /= scaled;
      dA_per_pt /= scaled;
      dO_per_pt /= scaled;
    }
  }

  const bool split_axes = cfg.terraforming_split_points_between_axes;

  // Mineral costs.
  out.duranium_per_point = std::max(0.0, cfg.terraforming_duranium_per_point);
  out.neutronium_per_point = std::max(0.0, cfg.terraforming_neutronium_per_point);

  const double cost_d = opt.ignore_mineral_costs ? 0.0 : out.duranium_per_point;
  const double cost_n = opt.ignore_mineral_costs ? 0.0 : out.neutronium_per_point;

  // Build per-colony point budgets (current minerals only, no replenishment).
  std::vector<ColonyBudget> budgets;
  budgets.reserve(st.colonies.size());

  for (const auto& [cid, c] : st.colonies) {
    if (c.body_id != body_id) continue;

    const double pts = sim.terraforming_points_per_day(c);

    ColonyBudget cb;
    cb.colony_id = cid;
    cb.pts_per_day = std::max(0.0, pts);

    auto itD = c.minerals.find("Duranium");
    auto itN = c.minerals.find("Neutronium");
    cb.dur_rem = (itD == c.minerals.end()) ? 0.0 : std::max(0.0, itD->second);
    cb.neu_rem = (itN == c.minerals.end()) ? 0.0 : std::max(0.0, itN->second);

    out.points_per_day += cb.pts_per_day;

    // Track inputs for UI.
    TerraformingColonyContribution cc;
    cc.colony_id = cid;
    cc.points_per_day = cb.pts_per_day;
    cc.duranium_available = cb.dur_rem;
    cc.neutronium_available = cb.neu_rem;
    out.colonies.push_back(cc);

    budgets.push_back(cb);
  }

  // Aggregate starting minerals only for colonies with points.
  for (const auto& b : budgets) {
    if (b.pts_per_day <= 1e-9) continue;
    out.duranium_available += b.dur_rem;
    out.neutronium_available += b.neu_rem;
  }

  auto compute_done = [&]() {
    bool temp_done = true;
    bool atm_done = true;
    bool o2_done = true;

    if (out.target_temp_k > 0.0 && dT_per_pt > 0.0) {
      temp_done = approx_equal(temp, out.target_temp_k, tolT);
    }
    if (out.target_atm > 0.0 && dA_per_pt > 0.0) {
      atm_done = approx_equal(atm, out.target_atm, tolA);
    }
    if (out.target_o2_atm > 0.0 && dO_per_pt > 0.0) {
      o2_done = approx_equal(o2, out.target_o2_atm, tolO);
      if (max_o2_frac > 0.0 && atm * max_o2_frac + tolO < out.target_o2_atm) o2_done = false;
    }
    return temp_done && atm_done && o2_done;
  };

  // Already complete?
  if (body->terraforming_complete || compute_done()) {
    out.ok = true;
    out.complete = true;
    out.days_to_complete = 0;
    out.end_temp_k = out.target_temp_k > 0.0 ? out.target_temp_k : temp;
    out.end_atm = out.target_atm > 0.0 ? out.target_atm : atm;
    out.end_o2_atm = out.target_o2_atm > 0.0 ? out.target_o2_atm : o2;
    return out;
  }

  // If there are no points/day at all, we can immediately mark as stalled.
  if (out.points_per_day <= 1e-9) {
    out.ok = true;
    out.complete = false;
    out.stalled = true;
    out.stall_reason = "No terraforming points/day (build Terraforming Facilities)";
    out.end_temp_k = temp;
    out.end_atm = atm;
    out.end_o2_atm = o2;
    return out;
  }

  const int max_days = std::max(0, opt.max_days);
  const int hard_cap = std::min(max_days, 2000000); // safety: avoid pathological loops

  double dur_start_total = 0.0;
  double neu_start_total = 0.0;
  for (const auto& b : budgets) {
    dur_start_total += b.dur_rem;
    neu_start_total += b.neu_rem;
  }


  for (int day = 0; day < hard_cap; ++day) {
    // Determine available points for this day, reduced by mineral shortages.
    double pts_total = 0.0;
    for (auto& b : budgets) {
      if (b.pts_per_day <= 1e-12) continue;

      double pts = b.pts_per_day; // dt=1 day

      double afford = 1.0;
      if (cost_d > 0.0) {
        const double need = pts * cost_d;
        if (need > 1e-12) afford = std::min(afford, clamp01(b.dur_rem / need));
      }
      if (cost_n > 0.0) {
        const double need = pts * cost_n;
        if (need > 1e-12) afford = std::min(afford, clamp01(b.neu_rem / need));
      }

      pts *= afford;
      if (pts <= 1e-12) continue;

      if (cost_d > 0.0) {
        b.dur_rem = std::max(0.0, b.dur_rem - pts * cost_d);
      }
      if (cost_n > 0.0) {
        b.neu_rem = std::max(0.0, b.neu_rem - pts * cost_n);
      }

      pts_total += pts;
    }

    out.points_applied += pts_total;
    out.days_simulated = day + 1;


    if (pts_total <= 1e-9) {
      out.ok = true;
      out.complete = false;
      out.stalled = true;

      if ((cost_d > 0.0 || cost_n > 0.0) && (dur_start_total > 1e-6 || neu_start_total > 1e-6)) {
        out.stall_reason = "Terraforming stalled: insufficient minerals";
      } else if (cost_d > 0.0 || cost_n > 0.0) {
        out.stall_reason = "Terraforming stalled: missing minerals (Duranium/Neutronium)";
      } else {
        out.stall_reason = "Terraforming stalled: no usable points/day";
      }

      break;
    }

    // Allocate points to axes.
    const bool needT = (out.target_temp_k > 0.0 && dT_per_pt > 1e-12 && !approx_equal(temp, out.target_temp_k, tolT));
    const bool needA = (out.target_atm > 0.0 && dA_per_pt > 1e-12 && !approx_equal(atm, out.target_atm, tolA));
    // O2 is optionally constrained by max fraction of total pressure. If the
    // current atmosphere is too low to ever reach the target within the cap,
    // allocate points to atmosphere first.
    const bool o2_atm_limited = (out.target_o2_atm > 0.0 && max_o2_frac > 0.0 &&
                                 (atm * max_o2_frac + tolO < out.target_o2_atm));
    const bool needO = (out.target_o2_atm > 0.0 && dO_per_pt > 1e-12 && !o2_atm_limited &&
                        !approx_equal(o2, out.target_o2_atm, tolO));

    double ptsT = 0.0;
    double ptsA = 0.0;
    double ptsO = 0.0;

    if (split_axes) {
      const int need_axes = static_cast<int>(needT) + static_cast<int>(needA) + static_cast<int>(needO);
      if (need_axes <= 0) {
        ptsT = 0.0;
        ptsA = 0.0;
        ptsO = 0.0;
      } else if (need_axes == 1) {
        ptsT = needT ? pts_total : 0.0;
        ptsA = needA ? pts_total : 0.0;
        ptsO = needO ? pts_total : 0.0;
      } else {
        // Prefer manual per-body weights when present; otherwise, allocate by remaining deltas.
        const double mT = needT ? std::max(0.0, body->terraforming_weight_temp) : 0.0;
        const double mA = needA ? std::max(0.0, body->terraforming_weight_atm) : 0.0;
        const double mO = needO ? std::max(0.0, body->terraforming_weight_o2) : 0.0;
        const double manual_sum = mT + mA + mO;

        double fracT = 0.0;
        double fracA = 0.0;
        double fracO = 0.0;

        if (manual_sum > 1e-12) {
          fracT = (needT && mT > 0.0) ? std::clamp(mT / manual_sum, 0.0, 1.0) : 0.0;
          fracA = (needA && mA > 0.0) ? std::clamp(mA / manual_sum, 0.0, 1.0) : 0.0;
          fracO = (needO && mO > 0.0) ? std::clamp(mO / manual_sum, 0.0, 1.0) : 0.0;
        } else {
          const double wT = needT ? (std::abs(out.target_temp_k - temp) / std::max(1e-12, dT_per_pt)) : 0.0;
          const double wA = needA ? (std::abs(out.target_atm - atm) / std::max(1e-12, dA_per_pt)) : 0.0;
          const double wO = needO ? (std::abs(out.target_o2_atm - o2) / std::max(1e-12, dO_per_pt)) : 0.0;
          const double denom = wT + wA + wO;
          fracT = (denom > 1e-12 && needT) ? std::clamp(wT / denom, 0.0, 1.0) : 0.0;
          fracA = (denom > 1e-12 && needA) ? std::clamp(wA / denom, 0.0, 1.0) : 0.0;
          fracO = (denom > 1e-12 && needO) ? std::clamp(wO / denom, 0.0, 1.0) : 0.0;
        }

        ptsT = pts_total * fracT;
        ptsA = pts_total * fracA;
        ptsO = pts_total * fracO;
      }
    } else {
      // Non-split mode: treat points as applying to all axes simultaneously.
      ptsT = pts_total;
      ptsA = pts_total;
      ptsO = pts_total;
    }

    if (needT && ptsT > 0.0) {
      temp = step_toward(temp, out.target_temp_k, ptsT * dT_per_pt);
    }
    if (needA && ptsA > 0.0) {
      atm = step_toward(atm, out.target_atm, ptsA * dA_per_pt);
      if (atm < 0.0) atm = 0.0;
    }

    // Keep oxygen consistent when atmosphere changes.
    if (o2 > atm) o2 = atm;

    if (needO && ptsO > 0.0) {
      const double target_o2 = (max_o2_frac > 0.0) ? std::min(out.target_o2_atm, atm * max_o2_frac)
                                                   : out.target_o2_atm;
      o2 = step_toward(o2, target_o2, ptsO * dO_per_pt);
      if (o2 < 0.0) o2 = 0.0;
      if (o2 > atm) o2 = atm;
    }

    if (compute_done()) {
      out.ok = true;
      out.complete = true;
      out.days_to_complete = day + 1;
      break;
    }
  }

  // Minerals consumed.
  double dur_end_total = 0.0;
  double neu_end_total = 0.0;
  for (const auto& b : budgets) {
    dur_end_total += b.dur_rem;
    neu_end_total += b.neu_rem;
  }
  out.duranium_consumed = std::max(0.0, dur_start_total - dur_end_total);
  out.neutronium_consumed = std::max(0.0, neu_start_total - neu_end_total);

  out.end_temp_k = temp;
  out.end_atm = atm;
  out.end_o2_atm = o2;

  // Truncation handling.
  if (out.ok && !out.complete && !out.stalled) {
    out.truncated = true;
    out.truncated_reason = "Exceeded max_days";
  }

  // If we never set ok=true in the loop (e.g., hard_cap==0), still mark ok.
  if (!out.ok) {
    out.ok = true;
    if (!out.complete && !out.stalled) {
      out.truncated = true;
      out.truncated_reason = "Exceeded max_days";
    }
  }

  return out;
}

} // namespace nebula4x
