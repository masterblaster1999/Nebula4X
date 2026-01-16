#pragma once

// Procedural "surface stamp" generation for celestial bodies.
//
// This module produces small deterministic ASCII stamps and lightweight flavor
// metadata (biome classification + quirky tags). It is intentionally:
// - Deterministic: stable for a given body id/attributes.
// - Cheap: stamps are small and suitable for UI rendering.
// - Pure: no mutation of game state; callers may cache results.
//
// Design intent:
//  - Provide underdeveloped parts of the game (exploration/colonization) with
//    richer "feel" without committing to full surface simulation.
//
// The stamp generator is deliberately "obscure": it blends a tiny plate-tectonic
// Voronoi field with domain-warped value noise and quantile sea-leveling to
// produce recognizable continents, mountains, ice caps, bands, and spots.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "nebula4x/core/entities.h"
#include "nebula4x/core/procgen_obscure.h"

namespace nebula4x::procgen_surface {

struct Quirk {
  std::string name;
  std::string desc;
};

struct Flavor {
  std::string biome;
  std::vector<Quirk> quirks;
  std::string stamp;
  std::string legend;
};

namespace detail {

inline std::uint64_t mix(std::uint64_t x) { return procgen_obscure::splitmix64(x); }

inline std::uint64_t hash_combine(std::uint64_t a, std::uint64_t b) {
  // Similar to boost::hash_combine but deterministic and constexpr-ish.
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
  for (int i = 0; i < std::max(1, octaves); ++i) {
    sum += amp * value_noise(seed + static_cast<std::uint64_t>(i) * 0x9e3779b97f4a7c15ULL, x * freq, y * freq);
    norm += amp;
    amp *= gain;
    freq *= lacunarity;
  }
  if (norm <= 1e-12) return 0.0;
  return sum / norm;
}

inline std::pair<double, double> domain_warp(std::uint64_t seed, double x, double y) {
  // Two-channel warp. Keeps the stamp "alive" without a heavy simulation.
  const double wx = fbm(seed ^ 0xA2F1B4C3D5E60719ULL, x, y, 3, 2.1, 0.52) - 0.5;
  const double wy = fbm(seed ^ 0xC0FFEE123456789BULL, x, y, 3, 2.1, 0.52) - 0.5;
  return {x + wx * 0.85, y + wy * 0.85};
}

inline std::uint64_t body_seed(const Body& b, std::uint64_t salt) {
  std::uint64_t s = 0xD6E8FEB86659FD93ULL;
  s = hash_combine(s, static_cast<std::uint64_t>(b.id));
  s = hash_combine(s, static_cast<std::uint64_t>(b.system_id));
  s = hash_combine(s, static_cast<std::uint64_t>(b.parent_body_id));
  s = hash_combine(s, static_cast<std::uint64_t>(static_cast<int>(b.type)));
  s = hash_combine(s, salt);
  return mix(s);
}

inline double safe_temp_k(const Body& b) {
  if (b.surface_temp_k > 0.0) return b.surface_temp_k;
  // When temp isn't present (mods / hand-authored), fall back to a sane neutral.
  return 288.0;
}

inline double safe_atm(const Body& b) {
  // The terraforming prototype uses target_atm even if atm is 0; we want the
  // stamp to have some "personality" even for barren worlds being terraformed.
  if (b.atmosphere_atm > 0.0) return b.atmosphere_atm;
  if (b.terraforming_target_atm > 0.0) return b.terraforming_target_atm * 0.15;
  return 0.0;
}

inline double orbit_au(const Body& b) { return (b.orbit_radius_mkm > 0.0) ? (b.orbit_radius_mkm / 149.6) : 0.0; }

inline std::string biome_for_terrestrial(const Body& b) {
  const double t = safe_temp_k(b);
  const double atm = safe_atm(b);

  if (atm < 0.01) {
    if (t < 170.0) return "Airless Ice Rock";
    if (t < 450.0) return "Airless Rock";
    return "Airless Ember";
  }

  // Moderation factor peaks near ~288K.
  const double moderate = clamp01(1.0 - std::fabs(t - 288.0) / 140.0);

  if (t < 190.0) return "Ice World";
  if (t < 240.0) return (moderate > 0.3) ? "Cold Ocean World" : "Frozen World";
  if (t < 320.0) {
    if (atm > 3.0) return "Temperate Super-Atmosphere";
    if (moderate > 0.72) return "Temperate World";
    if (moderate > 0.45) return "Dry Temperate World";
    return "Barren Temperate";
  }
  if (t < 420.0) return (atm > 1.5) ? "Greenhouse World" : "Hot Desert World";
  return (atm > 1.0) ? "Runaway Greenhouse" : "Inferno World";
}

inline double desired_water_fraction(const Body& b) {
  const double t = safe_temp_k(b);
  const double atm = safe_atm(b);

  if (b.type == BodyType::Asteroid) return 0.0;
  if (b.type == BodyType::Comet) return 0.10;
  if (atm < 0.01) return 0.0;

  // A soft climate heuristic:
  // - moderate temps => more surface liquids
  // - extremes => less
  // - thick atmospheres bias toward more "global" coverage
  const double moderate = clamp01(1.0 - std::fabs(t - 288.0) / 170.0);
  double w = 0.20 + 0.55 * moderate;
  if (t < 220.0) w = 0.35 + 0.45 * clamp01((220.0 - t) / 90.0); // ice oceans / frozen seas
  if (t > 360.0) w *= 0.55;
  if (atm > 2.5) w = std::min(0.85, w + 0.10);
  return std::clamp(w, 0.0, 0.88);
}

inline double desired_ice_strength(const Body& b) {
  const double t = safe_temp_k(b);
  // 1.0 around ~140K, fading to ~0 near 260K.
  return clamp01((260.0 - t) / 120.0);
}

inline double desired_desertness(const Body& b) {
  const double t = safe_temp_k(b);
  const double atm = safe_atm(b);
  if (atm < 0.01) return 1.0;
  double hot = clamp01((t - 310.0) / 140.0);
  if (atm > 2.0) hot *= 0.75; // thick atmospheres are not always deserts
  return hot;
}

struct Plate {
  double cx{0.0};
  double cy{0.0};
  double elev{0.0};
  double rough{0.0};
};

inline void tectonic_field(const Body& b, int w, int h, std::vector<double>& out_elev) {
  out_elev.assign(static_cast<std::size_t>(w * h), 0.0);

  const std::uint64_t seed = body_seed(b, 0x6D9D612A03E2A09BULL);
  procgen_obscure::HashRng rng(seed);

  const int cells = std::max(1, w * h);
  const int plates_n = std::clamp(cells / 60, 4, 12);

  std::vector<Plate> plates;
  plates.reserve(static_cast<std::size_t>(plates_n));
  for (int i = 0; i < plates_n; ++i) {
    Plate p;
    p.cx = rng.next_u01() * static_cast<double>(w);
    p.cy = rng.next_u01() * static_cast<double>(h);

    // A gentle bias toward oceanic plates so quantile sea-leveling has room to
    // "promote" continents based on water fraction.
    const double base = rng.range_real(-0.70, 0.95);
    p.elev = base;
    p.rough = rng.range_real(0.6, 1.4);
    plates.push_back(p);
  }

  const std::uint64_t nseed = body_seed(b, 0x0BADC0FFEE0DDF00ULL);

  auto dist2 = [&](double x0, double y0, double x1, double y1) {
    const double dx = x0 - x1;
    const double dy = y0 - y1;
    return dx * dx + dy * dy;
  };

  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      // Find nearest + second nearest plate (for boundaries).
      double d1 = 1e30, d2 = 1e30;
      int i1 = 0;

      const double fx = static_cast<double>(x) + 0.5;
      const double fy = static_cast<double>(y) + 0.5;

      for (int i = 0; i < plates_n; ++i) {
        const double d = dist2(fx, fy, plates[static_cast<std::size_t>(i)].cx, plates[static_cast<std::size_t>(i)].cy);
        if (d < d1) {
          d2 = d1;
          d1 = d;
          i1 = i;
        } else if (d < d2) {
          d2 = d;
        }
      }

      const Plate& p = plates[static_cast<std::size_t>(i1)];

      // Boundary "ridge" grows when nearest and second-nearest are close.
      const double delta = std::sqrt(std::max(0.0, d2)) - std::sqrt(std::max(0.0, d1));
      const double boundary = clamp01(1.0 - delta / 1.35);

      // Domain-warped FBM adds intra-plate texture.
      const double nx = (static_cast<double>(x) / std::max(1.0, static_cast<double>(w))) * 3.2;
      const double ny = (static_cast<double>(y) / std::max(1.0, static_cast<double>(h))) * 3.2;
      auto [wx, wy] = domain_warp(nseed ^ mix(static_cast<std::uint64_t>(i1)), nx, ny);
      const double noise = fbm(nseed, wx, wy, 4, 2.0, 0.52);

      // Compose elevation:
      // - plate elevation sets continental/oceanic baseline
      // - noise adds hills
      // - boundaries add mountain belts
      double elev = p.elev * 0.75 + (noise - 0.5) * 0.42 * p.rough + boundary * 0.85;

      // Slight equatorial bulge on larger planets to break symmetry.
      const double lat = std::abs((static_cast<double>(y) / std::max(1.0, static_cast<double>(h - 1))) * 2.0 - 1.0);
      elev += (1.0 - lat) * 0.07 * (fbm(nseed ^ 0x1234567ULL, nx * 0.7, ny * 0.7, 2, 2.0, 0.5) - 0.5);

      out_elev[static_cast<std::size_t>(y * w + x)] = elev;
    }
  }
}

inline double quantile(std::vector<double> v, double q01) {
  if (v.empty()) return 0.0;
  const double q = std::clamp(q01, 0.0, 1.0);
  const std::size_t n = v.size();
  const std::size_t idx = static_cast<std::size_t>(std::clamp(std::floor(q * static_cast<double>(n - 1)), 0.0,
                                                              static_cast<double>(n - 1)));
  std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(idx), v.end());
  return v[idx];
}

inline std::string stamp_with_border(const std::vector<std::string>& rows) {
  std::string out;
  const int h = static_cast<int>(rows.size());
  const int w = h > 0 ? static_cast<int>(rows[0].size()) : 0;

  auto append_line = [&](const std::string& s) {
    out += s;
    out += '\n';
  };

  append_line("+" + std::string(static_cast<std::size_t>(w), '-') + "+");
  for (int y = 0; y < h; ++y) {
    append_line("|" + rows[static_cast<std::size_t>(y)] + "|");
  }
  append_line("+" + std::string(static_cast<std::size_t>(w), '-') + "+");
  return out;
}

inline std::string stamp_terrestrial(const Body& b, int w, int h, std::string* legend_out) {
  std::vector<double> elev;
  tectonic_field(b, w, h, elev);

  const double water_frac = desired_water_fraction(b);
  // "Water" is the lower tail of elevations.
  const double sea_level = quantile(elev, water_frac);

  const double ice_strength = desired_ice_strength(b);
  const double desertness = desired_desertness(b);

  // Terrain palette.
  const char ocean_ch = '~';
  const char ice_ch = '*';
  const char mtn_ch = '^';
  const char land_ch = (desertness > 0.65) ? ':' : '.';
  const char hill_ch = (desertness > 0.65) ? ';' : ',';

  if (legend_out) {
    *legend_out = "Legend: ~ ocean   . land   , hills   ^ mountains   * ice";
    if (desertness > 0.65) *legend_out = "Legend: ~ (rare) seas   : desert   ; hills   ^ mountains   * ice";
    if (water_frac <= 0.02) *legend_out = "Legend: . rock   , broken terrain   ^ mountains   * frost";
  }

  // Determine a mountain threshold from the upper tail so every stamp has some relief.
  const double mtn_level = quantile(elev, 0.88);

  std::vector<std::string> rows;
  rows.reserve(static_cast<std::size_t>(h));

  procgen_obscure::HashRng rng(body_seed(b, 0xB16B00B5ULL));

  for (int y = 0; y < h; ++y) {
    std::string row;
    row.resize(static_cast<std::size_t>(w), land_ch);

    const double lat01 = (h <= 1) ? 0.0 : static_cast<double>(y) / static_cast<double>(h - 1);
    const double lat = std::abs(lat01 * 2.0 - 1.0); // 0 eq, 1 poles

    for (int x = 0; x < w; ++x) {
      const double e = elev[static_cast<std::size_t>(y * w + x)];

      // A small micro-noise used only for char variation (prevents large flat patches).
      const double micro = hash2_u01(body_seed(b, 0x51A1A1A1ULL), x, y);

      char c = land_ch;

      const bool is_ocean = (e <= sea_level + (micro - 0.5) * 0.02);
      if (is_ocean) c = ocean_ch;

      // Hills / mountains.
      if (e > sea_level + 0.12 && micro > 0.62) c = hill_ch;
      if (e > mtn_level && micro > 0.35) c = mtn_ch;

      // Ice caps scale with temperature (and respect deserts a bit).
      if (ice_strength > 0.02) {
        const double cap = 1.0 - (0.40 + 0.55 * ice_strength);
        if (lat > cap) {
          // In the "marginal" zone, only some cells become ice.
          const double chance = clamp01((lat - cap) / std::max(1e-6, (1.0 - cap)));
          if (micro < chance) c = ice_ch;
        }
      }

      row[static_cast<std::size_t>(x)] = c;
    }

    // Rare oddity: a single-character "rift" slash.
    if (desertness < 0.2 && water_frac > 0.15 && rng.next_u01() < 0.08) {
      const int cx = rng.range_int(2, std::max(2, w - 3));
      row[static_cast<std::size_t>(cx)] = '/';
    }

    rows.push_back(std::move(row));
  }

  return stamp_with_border(rows);
}

inline std::string stamp_gas_giant(const Body& b, int w, int h, std::string* legend_out) {
  const std::uint64_t seed = body_seed(b, 0x0DDC0FFEE0DDF00ULL);
  procgen_obscure::HashRng rng(seed);

  const int bands = std::clamp(4 + rng.range_int(0, 5), 4, 10);
  const double phase = rng.next_u01() * 6.283185307179586;

  const char bright = '=';
  const char mid = '-';
  const char dark = '_';
  const char storm = 'O';

  if (legend_out) *legend_out = "Legend: = bright bands   - mid   _ dark   O storm";

  std::vector<std::string> rows;
  rows.reserve(static_cast<std::size_t>(h));

  for (int y = 0; y < h; ++y) {
    std::string row;
    row.resize(static_cast<std::size_t>(w), mid);

    const double fy = (h <= 1) ? 0.0 : static_cast<double>(y) / static_cast<double>(h - 1);
    const double s = std::sin((fy * static_cast<double>(bands) * 6.283185307179586) + phase);
    const double band_base = 0.5 + 0.5 * s;

    for (int x = 0; x < w; ++x) {
      const double fx = (w <= 1) ? 0.0 : static_cast<double>(x) / static_cast<double>(w - 1);
      auto [wx, wy] = domain_warp(seed ^ 0xABCDEF1234ULL, fx * 2.8, fy * 2.0);
      const double n = fbm(seed, wx * 4.0, wy * 4.0, 3, 2.0, 0.55);

      const double v = 0.72 * band_base + 0.28 * n;

      char c = mid;
      if (v > 0.64) c = bright;
      else if (v < 0.38) c = dark;

      // Storms: rarer near poles.
      const double pole = std::abs(fy * 2.0 - 1.0);
      const double storm_prob = 0.016 * (1.0 - 0.75 * pole);
      if (n > 0.86 && rng.next_u01() < storm_prob) c = storm;

      row[static_cast<std::size_t>(x)] = c;
    }

    rows.push_back(std::move(row));
  }

  return stamp_with_border(rows);
}

inline std::string stamp_star(const Body& b, int w, int h, std::string* legend_out) {
  const std::uint64_t seed = body_seed(b, 0x5A17B0B5ULL);
  procgen_obscure::HashRng rng(seed);

  // A small ramp; keep it ASCII-friendly.
  const std::string ramp = " .:-=+*#%@";
  if (legend_out) *legend_out = "Legend: brightness ramp (center -> edge), spots are darker";

  std::vector<std::string> rows;
  rows.reserve(static_cast<std::size_t>(h));

  const double cx = (static_cast<double>(w) - 1.0) * 0.5;
  const double cy = (static_cast<double>(h) - 1.0) * 0.5;
  const double rmax = std::max(1e-6, std::min(cx, cy));

  for (int y = 0; y < h; ++y) {
    std::string row;
    row.resize(static_cast<std::size_t>(w), ' ');

    for (int x = 0; x < w; ++x) {
      const double dx = (static_cast<double>(x) - cx) / rmax;
      const double dy = (static_cast<double>(y) - cy) / rmax;
      const double rr = dx * dx + dy * dy;
      if (rr > 1.0) {
        row[static_cast<std::size_t>(x)] = ' ';
        continue;
      }

      const double r = std::sqrt(std::max(0.0, rr));
      const double base = 1.0 - r;

      const double n = fbm(seed ^ 0xFEE1DEADULL, dx * 3.5 + 1.0, dy * 3.5 + 1.0, 4, 2.0, 0.55);

      // Spots: high noise -> darker.
      const double spot = clamp01((n - 0.65) / 0.35);
      double v = base * 0.92 + n * 0.08;
      v *= (1.0 - 0.55 * spot);

      const int idx = std::clamp(static_cast<int>(std::floor(v * static_cast<double>(ramp.size() - 1))), 0,
                                 static_cast<int>(ramp.size() - 1));
      row[static_cast<std::size_t>(x)] = ramp[static_cast<std::size_t>(idx)];
    }

    rows.push_back(std::move(row));
  }

  // Add a rare flare symbol.
  if (rng.next_u01() < 0.12 && w >= 5 && h >= 3) {
    const int fx = rng.range_int(2, w - 3);
    const int fy = rng.range_int(1, h - 2);
    rows[static_cast<std::size_t>(fy)][static_cast<std::size_t>(fx)] = '!';
  }

  return stamp_with_border(rows);
}

inline std::string stamp_minor_body(const Body& b, int w, int h, std::string* legend_out) {
  const std::uint64_t seed = body_seed(b, 0xA57E0123ULL);
  procgen_obscure::HashRng rng(seed);

  const char rock = '#';
  const char regolith = '.';
  const char crater = 'o';
  const char ice = '*';

  if (legend_out) {
    *legend_out = "Legend: # rock   . regolith   o crater";
    if (b.type == BodyType::Comet) *legend_out += "   * ice";
  }

  std::vector<std::string> rows;
  rows.reserve(static_cast<std::size_t>(h));

  const double cx = (static_cast<double>(w) - 1.0) * 0.5;
  const double cy = (static_cast<double>(h) - 1.0) * 0.5;
  const double base_r = std::min(cx, cy) * rng.range_real(0.70, 0.92);

  // A small number of "crater seeds" using Worley-style nearest sites.
  const int sites_n = std::clamp(6 + rng.range_int(0, 10), 6, 18);
  std::vector<std::pair<double, double>> sites;
  sites.reserve(static_cast<std::size_t>(sites_n));
  for (int i = 0; i < sites_n; ++i) {
    sites.push_back({rng.next_u01() * static_cast<double>(w), rng.next_u01() * static_cast<double>(h)});
  }

  auto nearest_site_dist = [&](double x, double y) {
    double best = 1e30;
    for (const auto& s : sites) {
      const double dx = x - s.first;
      const double dy = y - s.second;
      best = std::min(best, dx * dx + dy * dy);
    }
    return std::sqrt(std::max(0.0, best));
  };

  for (int y = 0; y < h; ++y) {
    std::string row;
    row.resize(static_cast<std::size_t>(w), ' ');

    for (int x = 0; x < w; ++x) {
      const double dx = static_cast<double>(x) - cx;
      const double dy = static_cast<double>(y) - cy;
      const double r = std::sqrt(dx * dx + dy * dy);

      const double warp = (fbm(seed, (dx + 2.0) * 0.35, (dy + 2.0) * 0.35, 3, 2.0, 0.55) - 0.5) * 1.8;
      const double rr = r + warp;

      if (rr > base_r) {
        row[static_cast<std::size_t>(x)] = ' ';
        continue;
      }

      double d = nearest_site_dist(static_cast<double>(x) + 0.5, static_cast<double>(y) + 0.5);
      const double crater_field = clamp01(1.0 - d / 1.85);

      char c = (crater_field > 0.78) ? crater : ((crater_field > 0.58) ? regolith : rock);

      if (b.type == BodyType::Comet) {
        // Comets: add icy patches.
        const double icy = fbm(seed ^ 0xC011FEEULL, static_cast<double>(x) * 0.45, static_cast<double>(y) * 0.45, 3, 2.0, 0.55);
        if (icy > 0.76) c = ice;
      }

      row[static_cast<std::size_t>(x)] = c;
    }

    rows.push_back(std::move(row));
  }

  return stamp_with_border(rows);
}

inline std::vector<Quirk> quirks_for_body(const Body& b, const std::string& biome) {
  const std::uint64_t seed = body_seed(b, 0xD00DFEEDULL);
  procgen_obscure::HashRng rng(seed);

  struct Cand {
    const char* name;
    const char* desc;
    double w;
    bool enabled;
  };

  const double t = safe_temp_k(b);
  const double atm = safe_atm(b);
  const double au = orbit_au(b);

  const bool terrestrial = (b.type == BodyType::Planet || b.type == BodyType::Moon);
  const bool minor = (b.type == BodyType::Asteroid || b.type == BodyType::Comet);

  std::vector<Cand> cands;

  auto add = [&](const char* n, const char* d, double w, bool enabled) {
    if (!enabled) return;
    cands.push_back({n, d, std::max(0.0, w), true});
  };

  // Universal-ish.
  add("High Eccentricity", "Significant seasonal swings and variable solar input.", 1.0,
      std::abs(b.orbit_eccentricity) > 0.25);
  add("Resonant Orbit", "Orbital period suggests resonance with a nearby body.", 0.7,
      b.parent_body_id != kInvalidId && b.orbit_period_days > 0.0 && std::fmod(b.orbit_period_days, 2.0) < 0.02);
  add("Tidal Stresses", "Strong tidal forces drive fractures, heat, or volcanism.", 1.1,
      b.parent_body_id != kInvalidId && (b.type == BodyType::Moon || au < 0.45));
  add("Axial Tilt", "Unusual axial tilt creates extreme seasonal patterns.", 0.9,
      terrestrial && rng.next_u01() < 0.35);

  // Atmosphere-driven.
  add("Thin Atmosphere", "Sparse air; low insulation and minimal wind patterns.", 1.0, terrestrial && atm >= 0.01 && atm < 0.35);
  add("Dense Atmosphere", "Thick air; high drag and strong greenhouse effects.", 1.0, terrestrial && atm > 3.0);
  add("Toxic Clouds", "Reactive clouds corrode surfaces and hamper unshielded operations.", 0.8, terrestrial && atm > 0.35 && rng.next_u01() < 0.25);

  // Temperature-driven.
  add("Cryovolcanic", "Subsurface volatiles erupt as icy lava.", 1.0, terrestrial && t < 180.0 && atm >= 0.02);
  add("Magma Plains", "Widespread magma flows and incandescent basalt.", 1.0, terrestrial && t > 520.0);
  add("Tholin Haze", "Organic aerosols tint the atmosphere and dim surface light.", 0.8, terrestrial && t < 230.0 && atm > 0.15 && rng.next_u01() < 0.35);
  add("Glass Dunes", "Silica sands fused into drifting glassy sheets.", 0.8, terrestrial && t > 360.0 && atm > 0.15 && rng.next_u01() < 0.35);

  // Gravity-ish: if data present.
  add("Low Gravity", "Weak surface gravity; easy launches, difficult retention.", 0.6, terrestrial && b.mass_earths > 0.0 && b.mass_earths < 0.25);
  add("High Gravity", "Heavy gravity; punishing launches and compact atmospheres.", 0.7, terrestrial && b.mass_earths > 2.2);

  // Gas giants.
  add("Radiation Belts", "High-energy particle belts complicate close operations.", 1.0, b.type == BodyType::GasGiant && rng.next_u01() < 0.65);
  add("Great Storm", "A persistent storm system dominates a band.", 1.0, b.type == BodyType::GasGiant && rng.next_u01() < 0.45);
  add("Ring System", "A broad ring plane scatters light and debris.", 0.8, b.type == BodyType::GasGiant && rng.next_u01() < 0.55);

  // Stars.
  add("Flare Star", "Frequent flares increase radiation and EM noise.", 1.0, b.type == BodyType::Star && t > 4200.0 && rng.next_u01() < 0.45);
  add("Sunspot Cycle", "Cyclic spot activity modulates brightness and storms.", 0.8, b.type == BodyType::Star && rng.next_u01() < 0.65);

  // Minor bodies.
  add("Rubble Pile", "A loosely bound aggregate with low structural cohesion.", 0.9, minor && rng.next_u01() < 0.55);
  add("Metallic Body", "High metal fraction; dense and magnetically active.", 0.9, minor && rng.next_u01() < 0.35);
  add("Volatile-Rich", "High volatile fraction; prone to outgassing and jets.", 1.0, b.type == BodyType::Comet || (minor && rng.next_u01() < 0.25));
  add("Icy Caverns", "Subsurface voids filled with ancient ice.", 0.8, b.type == BodyType::Comet && rng.next_u01() < 0.55);

  // Biome-informed.
  add("Ocean Trenches", "Deep basins and active subduction zones.", 0.7, biome.find("Ocean") != std::string::npos && rng.next_u01() < 0.55);
  add("Dust Seas", "Vast dune fields driven by global winds.", 0.7, biome.find("Desert") != std::string::npos && rng.next_u01() < 0.65);

  // Pick 2-4 quirks, weighted, without replacement.
  const int want = std::clamp(2 + rng.range_int(0, 2), 0, 4);
  std::vector<Quirk> out;
  out.reserve(static_cast<std::size_t>(want));

  for (int pick = 0; pick < want && !cands.empty(); ++pick) {
    double sum = 0.0;
    for (const auto& c : cands) sum += c.w;

    if (sum <= 1e-9) break;

    double r = rng.next_u01() * sum;
    std::size_t idx = 0;
    for (std::size_t i = 0; i < cands.size(); ++i) {
      r -= cands[i].w;
      if (r <= 0.0) {
        idx = i;
        break;
      }
    }

    const Cand c = cands[idx];
    out.push_back({c.name, c.desc});
    cands.erase(cands.begin() + static_cast<std::ptrdiff_t>(idx));
  }

  // Keep stable ordering for UI scanning.
  std::sort(out.begin(), out.end(), [](const Quirk& a, const Quirk& b) { return a.name < b.name; });

  return out;
}

} // namespace detail

inline std::string biome_label(const Body& b) {
  switch (b.type) {
    case BodyType::Star: return "Star";
    case BodyType::GasGiant: return "Gas Giant";
    case BodyType::Asteroid: return "Asteroid";
    case BodyType::Comet: return "Comet";
    case BodyType::Moon:
    case BodyType::Planet:
    default: return detail::biome_for_terrestrial(b);
  }
}

inline std::string surface_stamp(const Body& b, int w = 26, int h = 12, std::string* legend_out = nullptr) {
  const int ww = std::clamp(w, 8, 64);
  const int hh = std::clamp(h, 6, 40);

  switch (b.type) {
    case BodyType::Star: return detail::stamp_star(b, ww, hh, legend_out);
    case BodyType::GasGiant: return detail::stamp_gas_giant(b, ww, hh, legend_out);
    case BodyType::Asteroid:
    case BodyType::Comet: return detail::stamp_minor_body(b, ww, hh, legend_out);
    case BodyType::Moon:
    case BodyType::Planet:
    default: return detail::stamp_terrestrial(b, ww, hh, legend_out);
  }
}

inline std::vector<Quirk> quirks(const Body& b) {
  return detail::quirks_for_body(b, biome_label(b));
}

inline Flavor flavor(const Body& b, int w = 26, int h = 12) {
  Flavor f;
  f.biome = biome_label(b);
  f.quirks = quirks(b);
  f.stamp = surface_stamp(b, w, h, &f.legend);
  return f;
}

} // namespace nebula4x::procgen_surface
