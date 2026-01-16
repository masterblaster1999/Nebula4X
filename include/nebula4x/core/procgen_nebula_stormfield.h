#pragma once

// Procedural nebula storm "cells" (spatial storm fields).
//
// Nebula4X models storms as a temporal intensity pulse per system
// (StarSystem::storm_*; Simulation::system_storm_intensity()). That makes storms
// feel uniform: every point in a system is equally affected.
//
// This module provides a deterministic, cheap 2D field sampled at arbitrary
// in-system coordinates. During an active storm, Simulation can combine the
// system-wide temporal pulse with this field to create moving storm "cells":
// calm pockets, violent cores, and drifting fronts.
//
// Design constraints:
//  - Deterministic: stable given (seed, position, storm_age, params).
//  - Cheap: piggybacks on nebula microfield value-noise + fBm + domain warp.
//  - Smooth in time: achieved by advecting the sampling position.

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "nebula4x/core/procgen_nebula_microfield.h"
#include "nebula4x/core/procgen_obscure.h"
#include "nebula4x/core/vec2.h"

namespace nebula4x::procgen_nebula_stormfield {

struct Params {
  // Typical size of storm cells (million-km).
  double cell_scale_mkm{1600.0};

  // Low-frequency domain-warp scale (million-km). If <=0, derived from cell_scale.
  double warp_scale_mkm{0.0};

  // How fast the storm field drifts (million-km per day).
  double drift_speed_mkm_per_day{220.0};

  // Blend between smooth blobs (0) and ridged/filamentary features (1).
  double filament_mix{0.55};

  // Contrast curve applied to the base field. >1 increases contrast.
  double sharpness{1.6};

  // Additional contrast applied after thresholding.
  double cell_contrast{1.35};

  // Threshold for "active" storm cores (0..1). Lower => more filled-in storms.
  double cell_threshold{0.30};

  // Optional swirl around system origin; helps storms read as coherent fronts.
  double swirl_strength{0.18};

  // Swirl scale (million-km). Larger => gentler rotation.
  double swirl_scale_mkm{8000.0};
};

namespace detail {

inline double clamp01(double x) { return std::clamp(x, 0.0, 1.0); }

inline Vec2 rotate(const Vec2& v, double ang_rad) {
  const double c = std::cos(ang_rad);
  const double s = std::sin(ang_rad);
  return Vec2{v.x * c - v.y * s, v.x * s + v.y * c};
}

// A deterministic drift direction derived from seed.
inline Vec2 drift_dir(std::uint64_t seed) {
  constexpr double kTwoPi = 6.283185307179586476925286766559;
  const double u = procgen_obscure::u01_from_u64(procgen_obscure::splitmix64(seed ^ 0xD00DFEEDu));
  const double a = u * kTwoPi;
  return Vec2{std::cos(a), std::sin(a)};
}

} // namespace detail

// Sample a normalized storm-cell field in [0,1].
//
// The field is intentionally centered around ~0.5 on average. Callers can
// remap it to an intensity multiplier (e.g., 1 + strength*(v-0.5)*2).
inline double sample_cell01(std::uint64_t seed, const Vec2& pos_mkm, double storm_age_days, const Params& p) {
  // Keep time numerically small and stable by using storm-relative age.
  const double t = std::clamp(storm_age_days, -3650.0, 3650.0);

  // Drift/advection: move the sampling position along a deterministic direction.
  const Vec2 dir = detail::drift_dir(seed);
  Vec2 q = pos_mkm + dir * (p.drift_speed_mkm_per_day * t);

  // Optional swirl around system origin (a tiny, smooth, radius-dependent twist).
  const double swirl_s = std::max(0.0, p.swirl_strength);
  if (swirl_s > 1e-9) {
    const double r = std::max(1e-9, q.length());
    const double scale = std::max(1000.0, p.swirl_scale_mkm);
    // Swirl angle grows with time, but decays with radius.
    const double ang = swirl_s * t * (scale / (scale + r));
    q = detail::rotate(q, ang);
  }

  // Reuse the microfield sampler (value-noise + fBm + domain-warp) with tuned params.
  procgen_nebula_microfield::Params mp;
  mp.scale_mkm = std::max(50.0, p.cell_scale_mkm);

  const double warp = (p.warp_scale_mkm > 1e-6) ? p.warp_scale_mkm : (mp.scale_mkm * 2.6);
  mp.warp_scale_mkm = std::max(50.0, warp);

  mp.filament_mix = std::clamp(p.filament_mix, 0.0, 1.0);
  mp.sharpness = std::clamp(p.sharpness, 0.25, 4.0);
  mp.strength = 1.0; // unused by sample_field01()

  double v = procgen_nebula_microfield::sample_field01(seed ^ 0xA5A5A5A5ULL, q, mp);

  // Convert the soft field into more "cellular" blobs by thresholding.
  const double thr = std::clamp(p.cell_threshold, 0.0, 0.95);
  if (thr > 1e-9) {
    v = (v - thr) / (1.0 - thr);
    v = detail::clamp01(v);
  }

  const double cc = std::clamp(p.cell_contrast, 0.25, 6.0);
  v = std::pow(detail::clamp01(v), cc);
  return detail::clamp01(v);
}

} // namespace nebula4x::procgen_nebula_stormfield
