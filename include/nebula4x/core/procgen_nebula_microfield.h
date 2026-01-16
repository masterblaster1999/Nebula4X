#pragma once

// Procedural "nebula microfields".
//
// Nebula4X historically modeled nebula interference as a *system-wide scalar*
// (StarSystem::nebula_density). That works, but it makes every "nebula system"
// behave uniformly: sensors and movement penalties apply equally everywhere.
//
// This module provides a deterministic, cheap 2D noise field that can be
// sampled at arbitrary in-system coordinates (mkm). The simulation can use it
// to introduce pockets/filaments of denser and clearer space so the System Map
// feels like terrain rather than a flat plane.
//
// Important design constraints:
//  - Deterministic: stable for a given (system_id, position, params).
//  - Cheap: value-noise + fBm + a small domain-warp.
//  - Pure: no mutation; callers can cache.

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "nebula4x/core/procgen_obscure.h"
#include "nebula4x/core/vec2.h"

namespace nebula4x::procgen_nebula_microfield {

struct Params {
  // Typical feature size of the microfield in million-km (mkm).
  // Smaller => finer filaments; larger => broader clouds.
  double scale_mkm{900.0};

  // Feature size for the low-frequency warp field (mkm).
  // Larger => gentler warps.
  double warp_scale_mkm{2600.0};

  // How strongly the microfield can deviate a system's base density.
  // 0 => disabled (returns ~base).
  double strength{0.28};

  // Blend between smooth clouds (0) and filamentary ridges (1).
  double filament_mix{0.65};

  // Post shaping power. >1 increases contrast, <1 flattens.
  double sharpness{1.25};
};

namespace detail {

inline std::uint64_t mix(std::uint64_t x) { return procgen_obscure::splitmix64(x); }

inline std::uint64_t hash_combine(std::uint64_t a, std::uint64_t b) {
  return mix(a ^ (b + 0x9e3779b97f4a7c15ULL + (a << 6) + (a >> 2)));
}

inline double clamp01(double x) { return std::clamp(x, 0.0, 1.0); }

inline double smoothstep(double t) {
  t = clamp01(t);
  return t * t * (3.0 - 2.0 * t);
}

inline double lerp(double a, double b, double t) { return a + (b - a) * t; }

inline double hash2_u01(std::uint64_t seed, int x, int y) {
  std::uint64_t h = seed;
  h = hash_combine(h, static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)));
  h = hash_combine(h, static_cast<std::uint64_t>(static_cast<std::uint32_t>(y)));
  return procgen_obscure::u01_from_u64(mix(h));
}

inline double value_noise(std::uint64_t seed, double x, double y) {
  // Value noise on integer lattice with smooth interpolation.
  const int x0 = static_cast<int>(std::floor(x));
  const int y0 = static_cast<int>(std::floor(y));
  const int x1 = x0 + 1;
  const int y1 = y0 + 1;

  const double tx = smoothstep(x - static_cast<double>(x0));
  const double ty = smoothstep(y - static_cast<double>(y0));

  const double v00 = hash2_u01(seed, x0, y0);
  const double v10 = hash2_u01(seed, x1, y0);
  const double v01 = hash2_u01(seed, x0, y1);
  const double v11 = hash2_u01(seed, x1, y1);

  const double a = lerp(v00, v10, tx);
  const double b = lerp(v01, v11, tx);
  return lerp(a, b, ty);
}

inline double fbm(std::uint64_t seed, double x, double y, int octaves, double lacunarity, double gain) {
  double amp = 0.5;
  double freq = 1.0;
  double sum = 0.0;
  double norm = 0.0;
  const int oct = std::max(1, octaves);
  for (int i = 0; i < oct; ++i) {
    sum += amp * value_noise(seed + static_cast<std::uint64_t>(i) * 0x9e3779b97f4a7c15ULL, x * freq, y * freq);
    norm += amp;
    amp *= gain;
    freq *= lacunarity;
  }
  if (norm <= 1e-12) return 0.0;
  return sum / norm;
}

inline Vec2 domain_warp(std::uint64_t seed, double x, double y) {
  // Two-channel warp in normalized coordinates.
  const double wx = fbm(seed ^ 0xA2F1B4C3D5E60719ULL, x, y, 3, 2.1, 0.52) - 0.5;
  const double wy = fbm(seed ^ 0xC0FFEE123456789BULL, x + 11.7, y - 7.9, 3, 2.1, 0.52) - 0.5;
  return Vec2{x + wx * 1.25, y + wy * 1.25};
}

inline double ridged(double n01) {
  // 0..1 -> 0..1, with ridges near the midline.
  const double t = 1.0 - std::abs(2.0 * n01 - 1.0);
  return clamp01(t);
}

} // namespace detail

// Sample a normalized microfield value in [0,1] at an in-system position.
//
// The returned value is a *shape* signal; callers typically remap it around a
// system's base nebula density.
inline double sample_field01(std::uint64_t seed, const Vec2& pos_mkm, const Params& p) {
  const double scale = std::max(10.0, p.scale_mkm);
  const double warp_scale = std::max(10.0, p.warp_scale_mkm);
  const double filament = std::clamp(p.filament_mix, 0.0, 1.0);
  const double sharp = std::clamp(p.sharpness, 0.25, 4.0);

  // Normalized coords.
  double x = pos_mkm.x / scale;
  double y = pos_mkm.y / scale;

  // Warp field is lower frequency.
  const double wx = pos_mkm.x / warp_scale;
  const double wy = pos_mkm.y / warp_scale;
  const Vec2 w = detail::domain_warp(seed ^ 0x5A17B3E57ULL, wx, wy);
  x += (w.x - wx) * (warp_scale / scale);
  y += (w.y - wy) * (warp_scale / scale);

  // Smooth clouds.
  const double n = detail::fbm(seed ^ 0xD1A5D1A5ULL, x, y, 5, 2.05, 0.52);

  // Filaments from ridged noise.
  const double r0 = detail::fbm(seed ^ 0xBADC0DEULL, x * 1.35 + 3.3, y * 1.35 - 7.1, 4, 2.15, 0.50);
  const double r = std::pow(detail::ridged(r0), 1.7);

  double v = detail::lerp(n, r, filament);
  v = std::pow(detail::clamp01(v), sharp);
  return detail::clamp01(v);
}

// Remap a sampled microfield around a base density.
//
// This keeps the *average* near base_density (since the microfield is centered
// around 0.5) while creating local pockets/filaments.
inline double local_density(double base_density, std::uint64_t seed, const Vec2& pos_mkm, const Params& p) {
  base_density = detail::clamp01(base_density);
  const double strength = std::clamp(p.strength, 0.0, 2.0);
  if (strength <= 1e-9) return base_density;
  if (base_density <= 1e-6) return 0.0;

  const double v = sample_field01(seed, pos_mkm, p);
  const double centered = (v - 0.5) * 2.0; // [-1, +1]

  // Variation peaks around mid densities, but never disappears completely.
  const double mid = 1.0 - std::abs(base_density - 0.5) * 2.0; // 0..1
  const double amp = strength * (0.10 + 0.55 * base_density) * (0.25 + 0.75 * detail::clamp01(mid));

  const double d = base_density + centered * amp;
  return detail::clamp01(d);
}

} // namespace nebula4x::procgen_nebula_microfield
