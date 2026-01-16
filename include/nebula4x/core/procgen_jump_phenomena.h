#pragma once

// Procedural "jump point phenomena".
//
// Jump points are one of the most important pieces of "terrain" in a space 4X,
// but they are often mechanically flat: a link is either known or unknown.
//
// This module generates a deterministic set of lightweight parameters for each
// JumpPoint:
//   - stability / turbulence / shear (0..1)
//   - survey difficulty multiplier
//   - transit hazard parameters (reserved for future integration)
//   - a short signature code and a tiny ASCII stamp for UI/debug tooltips
//
// Design constraints:
//   - Deterministic: stable for a given jump point id/system/position.
//   - Cheap: small value-noise + fBm + a small domain warp.
//   - Pure: does not mutate any game state.
//
// "Obscure" flavor: the stamp is derived from a warped multi-channel field
// so jump points feel like distinct "subspace weather" rather than identical nodes.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

#include "nebula4x/core/entities.h"
#include "nebula4x/core/procgen_obscure.h"

namespace nebula4x::procgen_jump_phenomena {

struct Phenomena {
  // Qualitative descriptors.
  double stability01{1.0};   // 1 = calm, 0 = wildly unstable
  double turbulence01{0.0};  // 1 = very turbulent
  double shear01{0.0};       // 1 = sharp gradients / filaments

  // Survey difficulty multiplier applied to SimConfig::jump_survey_points_required.
  // Values > 1 make surveying take longer.
  double survey_difficulty_mult{1.0};

  // Reserved for future integration: transit hazards.
  double hazard_chance01{0.0};        // base chance per transit (0..1)
  double hazard_damage_frac{0.0};     // approx fractional damage when hazard triggers (0..1)
  double misjump_dispersion_mkm{0.0}; // emergence scatter radius

  // Reserved for future integration: subsystem glitches.
  double subsystem_glitch_chance01{0.0};
  double subsystem_glitch_severity01{0.0};

  // UI/debug helpers.
  std::string signature_code; // e.g. JP-1A2B-3C4D
  std::string stamp;          // tiny ASCII thumbnail
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
  // Two-channel warp. The specific constants are arbitrary but stable.
  const double wx = fbm(seed ^ 0xA2F1B4C3D5E60719ULL, x, y, 3, 2.15, 0.52) - 0.5;
  const double wy = fbm(seed ^ 0xC0FFEE123456789BULL, x + 11.7, y - 7.9, 3, 2.15, 0.52) - 0.5;
  return Vec2{x + wx * 1.35, y + wy * 1.35};
}

inline double ridged01(double n01) {
  // 0..1 -> 0..1 ridge response.
  const double t = 1.0 - std::abs(2.0 * n01 - 1.0);
  return clamp01(t);
}

inline std::uint64_t jump_seed(const JumpPoint& jp, std::uint64_t salt) {
  std::uint64_t s = 0x9B4D0F1A6C25E3D7ULL;
  s = hash_combine(s, static_cast<std::uint64_t>(jp.id));
  s = hash_combine(s, static_cast<std::uint64_t>(jp.system_id));
  s = hash_combine(s, static_cast<std::uint64_t>(jp.linked_jump_id));

  // Quantize position so tiny float drift doesn't change the field.
  const std::int64_t qx = static_cast<std::int64_t>(std::llround(jp.position_mkm.x * 10.0));
  const std::int64_t qy = static_cast<std::int64_t>(std::llround(jp.position_mkm.y * 10.0));
  s = hash_combine(s, static_cast<std::uint64_t>(qx));
  s = hash_combine(s, static_cast<std::uint64_t>(qy));

  s = hash_combine(s, salt);
  return mix(s);
}

inline std::string signature_code(const JumpPoint& jp) {
  const std::uint64_t s = jump_seed(jp, 0x1CEB00DAULL);
  const std::uint32_t v = static_cast<std::uint32_t>((s >> 32) ^ (s & 0xFFFFFFFFu));
  const std::string h = procgen_obscure::hex_n(v, 8);
  return std::string("JP-") + h.substr(0, 4) + "-" + h.substr(4, 4);
}

inline double grad_mag01(std::uint64_t seed, double x, double y) {
  // Finite difference gradient magnitude, normalized to a rough 0..1 range.
  const double e = 0.35;
  const double cx = value_noise(seed, x, y);
  const double dx = value_noise(seed, x + e, y) - cx;
  const double dy = value_noise(seed, x, y + e) - cx;
  const double g = std::sqrt(dx * dx + dy * dy);
  return clamp01(g * 3.25);
}

inline std::string make_stamp(std::uint64_t seed, double x0, double y0) {
  // Tiny thumbnail: 16x8 + frame.
  constexpr int W = 16;
  constexpr int H = 8;

  std::string out;
  out.reserve((W + 2) * (H + 2) + (H + 1));

  auto put_line = [&](const std::string& s) {
    out += s;
    out.push_back('\n');
  };

  put_line("+----------------+");
  for (int y = 0; y < H; ++y) {
    std::string row;
    row.reserve(W + 2);
    row.push_back('|');
    for (int x = 0; x < W; ++x) {
      const double fx = x0 + (static_cast<double>(x) / static_cast<double>(W - 1)) * 2.2;
      const double fy = y0 + (static_cast<double>(y) / static_cast<double>(H - 1)) * 1.3;

      const Vec2 w = domain_warp(seed ^ 0xBADC0FFEEULL, fx, fy);
      const double n = fbm(seed ^ 0xD1A5D1A5ULL, w.x, w.y, 4, 2.05, 0.52);
      const double r = ridged01(fbm(seed ^ 0xDEADBEEFULL, w.x * 1.25, w.y * 1.25, 3, 2.2, 0.50));
      const double g = grad_mag01(seed ^ 0x1234567ULL, w.x * 1.6, w.y * 1.6);

      // Character palette:
      //   '.' = calm
      //   '~' = turbulence
      //   '#' = shear filament
      //   '*' = cusp / critical knot
      char c = '.';
      if (n > 0.70) c = '~';
      if (r > 0.72 || g > 0.72) c = '#';
      if ((r > 0.78 && n > 0.72) || g > 0.85) c = '*';
      row.push_back(c);
    }
    row.push_back('|');
    put_line(row);
  }
  // Strip final newline for easier UI embedding.
  out += "+----------------+";
  return out;
}

} // namespace detail

inline Phenomena generate(const JumpPoint& jp) {
  Phenomena p;

  const std::uint64_t seed = detail::jump_seed(jp, 0xC6A4A7935BD1E995ULL);

  // Normalize coordinates from position.
  // This doesn't attempt real physics; it just ensures nearby jump points feel
  // somewhat "related" within a system.
  const double scale = 950.0;
  double x = jp.position_mkm.x / scale;
  double y = jp.position_mkm.y / scale;

  // Warp on a lower frequency.
  const Vec2 w0 = detail::domain_warp(seed ^ 0x5A17B3E57ULL, x * 0.45, y * 0.45);
  x += (w0.x - x * 0.45) * 1.15;
  y += (w0.y - y * 0.45) * 1.15;

  const double turb = detail::clamp01(detail::fbm(seed ^ 0xABCDEF111ULL, x, y, 5, 2.07, 0.53));
  const double ridge = detail::clamp01(detail::ridged01(detail::fbm(seed ^ 0xDEAD1234ULL, x * 1.25 + 2.3, y * 1.25 - 3.9, 4, 2.15, 0.50)));
  const double shear = detail::clamp01(0.55 * ridge + 0.45 * detail::grad_mag01(seed ^ 0x0F00DULL, x * 1.6, y * 1.6));

  // Stability is inversely related to turbulence and shear.
  double stability = 1.0 - (0.62 * turb + 0.38 * shear);
  stability = detail::clamp01(stability);

  // Survey difficulty: calm points are easy, highly sheared/turbulent points take longer.
  const double complexity = detail::clamp01((turb + shear + (1.0 - stability)) / 3.0);
  const double shaped = std::pow(complexity, 1.15);
  const double difficulty_mult = detail::lerp(0.80, 2.25, shaped);

  // Transit hazard parameters are generated but not wired into simulation yet.
  const double hazard = detail::clamp01(0.15 + 0.80 * (0.55 * turb + 0.45 * shear) * (1.0 - 0.35 * stability));

  p.stability01 = stability;
  p.turbulence01 = turb;
  p.shear01 = shear;
  p.survey_difficulty_mult = difficulty_mult;

  p.hazard_chance01 = detail::clamp01(hazard * 0.55);
  p.hazard_damage_frac = detail::clamp01(0.02 + 0.18 * hazard);
  p.misjump_dispersion_mkm = std::max(0.0, 10.0 + 140.0 * hazard);

  p.subsystem_glitch_chance01 = detail::clamp01(0.05 + 0.45 * hazard);
  p.subsystem_glitch_severity01 = detail::clamp01(0.10 + 0.60 * hazard);

  p.signature_code = detail::signature_code(jp);

  // Seed stamp location with a deterministic offset so stamps differ even for
  // nearby points.
  const double sx = (procgen_obscure::u01_from_u64(seed ^ 0x1111ULL) - 0.5) * 3.0;
  const double sy = (procgen_obscure::u01_from_u64(seed ^ 0x2222ULL) - 0.5) * 3.0;
  p.stamp = detail::make_stamp(seed, x + sx, y + sy);

  return p;
}

} // namespace nebula4x::procgen_jump_phenomena
