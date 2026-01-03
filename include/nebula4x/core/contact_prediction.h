#pragma once

#include <algorithm>
#include <cmath>

#include "nebula4x/core/entities.h"

namespace nebula4x {

// A small helper structure returned by predict_contact_position().
//
// This is intentionally simple: it exists to support fog-of-war gameplay
// where ships may need to pursue a 'lost' contact using the last two
// observed positions as a constant-velocity estimate.
struct ContactPrediction {
  // Days since the contact was last seen (now_day - last_seen_day, clamped >= 0).
  int age_days{0};

  // How many days of extrapolation were actually applied.
  // (clamped to max_extrap_days).
  int extrapolated_days{0};

  // True if the contact had a valid previous snapshot to estimate velocity.
  bool has_velocity{false};

  // Estimated velocity in mkm/day (only valid if has_velocity is true).
  Vec2 velocity_mkm_per_day{0.0, 0.0};

  // Predicted position at now_day, using extrapolated_days.
  Vec2 predicted_position_mkm{0.0, 0.0};
};

// Predict a contact position at 'now_day' using a constant-velocity estimate.
//
// - If the contact does not have a valid previous snapshot (prev_seen_day),
//   predicted_position_mkm will be last_seen_position_mkm and has_velocity=false.
// - Extrapolation is clamped to max_extrap_days to avoid chasing stale tracks
//   forever.
inline ContactPrediction predict_contact_position(const Contact& c, int now_day, int max_extrap_days) {
  ContactPrediction out;
  if (now_day < 0) now_day = 0;
  if (max_extrap_days < 0) max_extrap_days = 0;

  out.age_days = std::max(0, now_day - c.last_seen_day);
  out.extrapolated_days = std::clamp(out.age_days, 0, max_extrap_days);

  out.predicted_position_mkm = c.last_seen_position_mkm;

  if (c.prev_seen_day > 0 && c.prev_seen_day < c.last_seen_day) {
    const int dt = (c.last_seen_day - c.prev_seen_day);
    if (dt > 0) {
      const Vec2 v = (c.last_seen_position_mkm - c.prev_seen_position_mkm) * (1.0 / (double)dt);
      if (std::isfinite(v.x) && std::isfinite(v.y)) {
        out.has_velocity = true;
        out.velocity_mkm_per_day = v;
        out.predicted_position_mkm = c.last_seen_position_mkm + v * (double)out.extrapolated_days;
      }
    }
  }

  return out;
}

} // namespace nebula4x
