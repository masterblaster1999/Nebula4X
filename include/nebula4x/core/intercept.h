#pragma once

#include "nebula4x/core/vec2.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace nebula4x {

// Lead-pursuit / intercept helper.
//
// Solves for a time t >= 0 such that a pursuer starting at `pursuer_pos_mkm`
// moving at constant speed `pursuer_speed_mkm_per_day` can reach within
// `desired_range_mkm` of a target starting at `target_pos_mkm` with constant
// velocity `target_velocity_mkm_per_day`.
//
// This is used by ship AI/order execution to "lead" moving targets instead of
// tail-chasing the current position.
struct InterceptAim {
  // True when a non-negative intercept solution exists.
  bool has_solution{false};

  // True when the computed solution time exceeded max_lead_days and we clamped
  // the aim point to a shorter lead.
  bool clamped{false};

  // Raw (unclamped) solution time in days.
  double solution_time_days{0.0};

  // Time used to compute the aim position (may be clamped).
  double aim_time_days{0.0};

  // Target position at aim_time_days.
  Vec2 aim_position_mkm{0.0, 0.0};
};

namespace detail {
inline bool finite_vec2(const Vec2& v) {
  return std::isfinite(v.x) && std::isfinite(v.y);
}
inline double dot(const Vec2& a, const Vec2& b) { return a.x * b.x + a.y * b.y; }
} // namespace detail

// Compute an intercept aim point.
//
// If no solution exists, returns {has_solution=false, aim_position=target_pos}.
inline InterceptAim compute_intercept_aim(const Vec2& pursuer_pos_mkm,
                                         double pursuer_speed_mkm_per_day,
                                         const Vec2& target_pos_mkm,
                                         const Vec2& target_velocity_mkm_per_day,
                                         double desired_range_mkm,
                                         double max_lead_days) {
  InterceptAim out;
  out.aim_position_mkm = target_pos_mkm;

  desired_range_mkm = std::max(0.0, desired_range_mkm);
  const double s = std::max(0.0, pursuer_speed_mkm_per_day);
  if (s <= 1e-12) return out;

  if (!detail::finite_vec2(pursuer_pos_mkm) || !detail::finite_vec2(target_pos_mkm) ||
      !detail::finite_vec2(target_velocity_mkm_per_day)) {
    return out;
  }

  const Vec2 d = target_pos_mkm - pursuer_pos_mkm;
  const double dist = d.length();
  if (!std::isfinite(dist)) return out;

  if (dist <= desired_range_mkm + 1e-9) {
    out.has_solution = true;
    out.solution_time_days = 0.0;
    out.aim_time_days = 0.0;
    out.aim_position_mkm = target_pos_mkm;
    return out;
  }

  const double v2 = detail::dot(target_velocity_mkm_per_day, target_velocity_mkm_per_day);
  const double a = v2 - s * s;
  const double b = 2.0 * (detail::dot(d, target_velocity_mkm_per_day) - s * desired_range_mkm);
  const double c = dist * dist - desired_range_mkm * desired_range_mkm;

  const double eps = 1e-12;

  double best_t = std::numeric_limits<double>::infinity();
  auto consider = [&](double t) {
    if (!std::isfinite(t)) return;
    if (t < -1e-9) return;
    best_t = std::min(best_t, std::max(0.0, t));
  };

  if (std::abs(a) <= eps) {
    // Linear (or degenerate) case.
    if (std::abs(b) <= eps) {
      // No meaningful solution (c>0 means we're outside desired_range and the
      // target is effectively moving away at the same speed along the line).
      return out;
    }
    consider(-c / b);
  } else {
    const double disc = b * b - 4.0 * a * c;
    if (disc < -1e-9) return out;
    const double disc_clamped = (disc < 0.0) ? 0.0 : disc;
    const double sqrt_disc = std::sqrt(disc_clamped);

    consider((-b - sqrt_disc) / (2.0 * a));
    consider((-b + sqrt_disc) / (2.0 * a));
  }

  if (!std::isfinite(best_t)) return out;

  out.has_solution = true;
  out.solution_time_days = best_t;
  out.aim_time_days = best_t;

  if (max_lead_days > 0.0 && out.aim_time_days > max_lead_days) {
    out.aim_time_days = max_lead_days;
    out.clamped = true;
  }

  out.aim_position_mkm = target_pos_mkm + target_velocity_mkm_per_day * out.aim_time_days;
  if (!detail::finite_vec2(out.aim_position_mkm)) {
    // Defensive: don't propagate NaNs.
    out = InterceptAim{};
    out.aim_position_mkm = target_pos_mkm;
  }

  return out;
}

} // namespace nebula4x
