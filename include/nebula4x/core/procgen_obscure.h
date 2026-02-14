#pragma once

// Obscure procedural generation helpers.
//
// This header provides a small, deterministic "lore/noise" toolkit used to
// make procedurally generated points-of-interest feel less repetitive.
//
// The intent is *not* simulation correctness; it's flavor:
//  - stable short signatures (useful for UI/debugging)
//  - tiny glyphs derived from a 1D cellular automaton (Rule 30)
//  - lightweight name + blurb generators for anomalies and caches
//
// Everything here is deterministic given ids/kinds and does not depend on any
// global RNG state.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

#include "nebula4x/core/entities.h"
#include "nebula4x/core/ids.h"
#include "nebula4x/util/hash_rng.h"

namespace nebula4x::procgen_obscure {

// --- low-level deterministic mixing / RNG -------------------------------------

// splitmix64: fast deterministic mixing suitable for procedural noise.
//
// Implementation is centralized in nebula4x::util so that all simulation and
// procedural systems share the exact same mixer.
inline std::uint64_t splitmix64(std::uint64_t x) { return ::nebula4x::util::splitmix64(x); }

inline double u01_from_u64(std::uint64_t x) { return ::nebula4x::util::u01_from_u64(x); }

using HashRng = ::nebula4x::util::HashRng;

// 64-bit FNV-1a for stable hashing of kind tags.
inline std::uint64_t fnv1a_64(std::string_view s) {
  std::uint64_t h = 14695981039346656037ULL;  // offset basis
  for (unsigned char c : s) {
    h ^= static_cast<std::uint64_t>(c);
    h *= 1099511628211ULL;  // prime
  }
  return h;
}

inline std::string hex_n(std::uint64_t x, int n) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  const int nn = std::clamp(n, 1, 16);
  std::string out;
  out.resize(static_cast<std::size_t>(nn));
  for (int i = nn - 1; i >= 0; --i) {
    out[static_cast<std::size_t>(i)] = kHex[static_cast<std::size_t>(x & 0xFULL)];
    x >>= 4;
  }
  return out;
}

// --- elementary cellular automaton glyphs -------------------------------------

// Rule 30, using wrap-around neighbors. For each cell, the next state is:
//   left XOR (center OR right).
//
// With bit-parallel operations, we can evolve 64 cells at once.
inline std::uint64_t eca_rule30_step(std::uint64_t row) {
  const std::uint64_t left = (row << 1) | (row >> 63);
  const std::uint64_t right = (row >> 1) | (row << 63);
  return left ^ (row | right);
}

inline std::string glyph8_from_seed(std::uint64_t seed) {
  // Use a mixed seed as the initial row so small id changes produce different
  // glyphs.
  std::uint64_t row = splitmix64(seed ^ 0xD1B54A32D192ED03ULL);

  std::string out;
  out.reserve(8 * 9);
  for (int y = 0; y < 8; ++y) {
    const std::uint8_t bits = static_cast<std::uint8_t>(row & 0xFFu);
    for (int x = 7; x >= 0; --x) {
      out.push_back(((bits >> x) & 1u) ? '#' : '.');
    }
    if (y != 7) out.push_back('\n');
    row = eca_rule30_step(row);
  }
  return out;
}

// --- signatures ---------------------------------------------------------------

inline std::uint64_t anomaly_seed(const Anomaly& a) {
  std::uint64_t s = 0x6D0F27BD9C2B3F61ULL;
  s ^= static_cast<std::uint64_t>(a.id) * 0x9e3779b97f4a7c15ULL;
  s ^= static_cast<std::uint64_t>(a.system_id) * 0xbf58476d1ce4e5b9ULL;
  s ^= static_cast<std::uint64_t>(a.kind) * 0x94d049bb133111ebULL;
  // If this is part of a lead chain, keep a coherent theme across the chain.
  if (a.origin_anomaly_id != kInvalidId) {
    s ^= static_cast<std::uint64_t>(a.origin_anomaly_id) * 0x2545f4914f6cdd1dULL;
  }
  s ^= static_cast<std::uint64_t>(std::max(0, a.lead_depth)) * 0x27d4eb2f165667c5ULL;
  return splitmix64(s);
}

inline std::string anomaly_signature_code(const Anomaly& a) {
  const std::uint64_t s = anomaly_seed(a);
  const std::uint32_t v = static_cast<std::uint32_t>((s >> 32) ^ (s & 0xFFFFFFFFu));
  const std::string h = hex_n(v, 8);
  return h.substr(0, 4) + "-" + h.substr(4, 4);
}

inline std::string anomaly_signature_glyph(const Anomaly& a) {
  return glyph8_from_seed(anomaly_seed(a));
}

// --- anomaly lead-chain helpers (root id + progress counting) ---
//
// These utilities operate on the anomaly map and are used by both simulation
// and UI layers to compute chain-local progression without adding new save state.
inline Id anomaly_chain_root_id(const std::unordered_map<Id, Anomaly>& anomalies, Id anomaly_id) {
  if (anomaly_id == kInvalidId) return kInvalidId;
  Id cur = anomaly_id;
  std::array<Id, 16> seen{};
  int seen_n = 0;
  for (int step = 0; step < 16; ++step) {
    if (cur == kInvalidId) break;
    bool dup = false;
    for (int i = 0; i < seen_n; ++i) {
      if (seen[static_cast<std::size_t>(i)] == cur) {
        dup = true;
        break;
      }
    }
    if (dup) break;
    if (seen_n < static_cast<int>(seen.size())) {
      seen[static_cast<std::size_t>(seen_n++)] = cur;
    }

    auto it = anomalies.find(cur);
    if (it == anomalies.end()) break;
    const Id parent = it->second.origin_anomaly_id;
    if (parent == kInvalidId || parent == cur) break;
    cur = parent;
  }
  return cur;
}

inline int faction_resolved_anomaly_chain_count(const std::unordered_map<Id, Anomaly>& anomalies,
                                                Id faction_id,
                                                Id root_anomaly_id) {
  if (faction_id == kInvalidId || root_anomaly_id == kInvalidId) return 0;
  int count = 0;
  for (const auto& [_, a] : anomalies) {
    if (!a.resolved) continue;
    if (a.resolved_by_faction_id != faction_id) continue;
    const Id root = anomaly_chain_root_id(anomalies, a.id);
    if (root == root_anomaly_id) ++count;
  }
  return count;
}

// A schematic fragment is a per-(anomaly, component) fingerprint.
// This lets exploration award *partial* reverse-engineering progress with
// a bit of flavor, without storing any additional entity state.
inline std::uint64_t schematic_fragment_seed(const Anomaly& a, std::string_view component_id) {
  std::uint64_t s = anomaly_seed(a);
  s ^= fnv1a_64(component_id) * 0xD6E8FEB86659FD93ULL;
  s ^= 0x9A1F3B0C2D4E5F61ULL;
  return splitmix64(s);
}

inline std::string schematic_fragment_code(const Anomaly& a, std::string_view component_id) {
  const std::uint64_t s = schematic_fragment_seed(a, component_id);
  const std::uint32_t v = static_cast<std::uint32_t>((s >> 32) ^ (s & 0xFFFFFFFFu));
  const std::string h = hex_n(v, 8);
  return h.substr(0, 4) + "-" + h.substr(4, 4);
}

inline std::string schematic_fragment_glyph(const Anomaly& a, std::string_view component_id) {
  return glyph8_from_seed(schematic_fragment_seed(a, component_id));
}

inline std::uint64_t wreck_seed(const Wreck& w, std::string_view tag = {}) {
  std::uint64_t s = 0xCACECA5E5EED5EEDULL;
  s ^= static_cast<std::uint64_t>(w.id) * 0x9e3779b97f4a7c15ULL;
  s ^= static_cast<std::uint64_t>(w.system_id) * 0xbf58476d1ce4e5b9ULL;
  s ^= static_cast<std::uint64_t>(w.kind) * 0x94d049bb133111ebULL;
  s ^= fnv1a_64(tag) * 0x2545f4914f6cdd1dULL;
  return splitmix64(s);
}

inline std::string wreck_signature_code(const Wreck& w, std::string_view tag = {}) {
  const std::uint64_t s = wreck_seed(w, tag);
  const std::uint32_t v = static_cast<std::uint32_t>((s >> 32) ^ (s & 0xFFFFFFFFu));
  const std::string h = hex_n(v, 8);
  return h.substr(0, 4) + "-" + h.substr(4, 4);
}

inline std::string wreck_signature_glyph(const Wreck& w, std::string_view tag = {}) {
  return glyph8_from_seed(wreck_seed(w, tag));
}

// --- lightweight name + lore generators ---------------------------------------

template <std::size_t N>
inline const char* pick_from(const std::array<const char*, N>& a, HashRng& rng) {
  if constexpr (N == 0) {
    return "";
  } else {
    const int idx = rng.range_int(0, static_cast<int>(N) - 1);
    return a[static_cast<std::size_t>(idx)];
  }
}

inline std::string anomaly_theme_label(const Anomaly& a) {
  // Keep chains coherent by keying the theme off the origin anomaly when present.
  const std::uint64_t key = (a.origin_anomaly_id != kInvalidId) ? static_cast<std::uint64_t>(a.origin_anomaly_id)
                                                                : static_cast<std::uint64_t>(a.id);
  HashRng rng(splitmix64(key ^ 0xA24BAED4963EE407ULL));

  static constexpr std::array<const char*, 24> kThemes = {
      "Cinder Choir",     "Glass Spiral",    "Eidolon Archive", "Helix Reliquary",
      "Aurora Lattice",   "Saffron Engine",  "Null Orchard",    "Pale Cathedral",
      "Vanta Circuit",    "Obsidian Canticle","Thorn Paradox",   "Signal Monastery",
      "Murmur Vault",     "Starlace Grotto", "Echo Reservoir",  "Kite Meridian",
      "Iron Psalm",       "Blue Wound",      "Gilded Aperture", "Sable Compass",
      "Hollow Index",     "Rift Lantern",    "Cobalt Basilica", "Dust Prophet",
  };

  return std::string(pick_from(kThemes, rng));
}

// Coarse domain for an anomaly theme. This is used to bias
// procedural rewards (e.g., schematic fragments) so a chain of related
// anomalies tends to point toward similar component types.
enum class ThemeDomain : std::uint8_t {
  Sensors = 0,
  Weapons = 1,
  Propulsion = 2,
  Industry = 3,
  Energy = 4,
};

inline ThemeDomain anomaly_theme_domain(const Anomaly& a) {
  const std::uint64_t key = (a.origin_anomaly_id != kInvalidId) ? static_cast<std::uint64_t>(a.origin_anomaly_id)
                                                                : static_cast<std::uint64_t>(a.id);
  const std::uint64_t h = splitmix64(key ^ 0x5B2C1F0E9D8A7C63ULL);
  return static_cast<ThemeDomain>(static_cast<std::uint8_t>(h % 5ULL));
}

inline const char* theme_domain_label(ThemeDomain d) {
  switch (d) {
    case ThemeDomain::Sensors: return "Sensors";
    case ThemeDomain::Weapons: return "Weapons";
    case ThemeDomain::Propulsion: return "Propulsion";
    case ThemeDomain::Industry: return "Industry";
    case ThemeDomain::Energy: return "Energy";
    default: return "Unknown";
  }
}

// Lightweight deterministic scan profile for anomaly triage.
// This is a flavor mechanic: it provides a stable "readout" that can be used by
// UI, logs and mission text to hint at risk/shape without storing extra state.
enum class AnomalyResonanceBand : std::uint8_t {
  Quiescent = 0,
  Harmonic = 1,
  Fractured = 2,
  Chaotic = 3,
  NullLocked = 4,
};

inline const char* anomaly_resonance_band_label(AnomalyResonanceBand b) {
  switch (b) {
    case AnomalyResonanceBand::Quiescent: return "Quiescent";
    case AnomalyResonanceBand::Harmonic: return "Harmonic";
    case AnomalyResonanceBand::Fractured: return "Fractured";
    case AnomalyResonanceBand::Chaotic: return "Chaotic";
    case AnomalyResonanceBand::NullLocked: return "Null-Locked";
    default: return "Unknown";
  }
}

struct AnomalyScanReadout {
  AnomalyResonanceBand resonance{AnomalyResonanceBand::Quiescent};
  ThemeDomain focus_domain{ThemeDomain::Sensors};
  int coherence_pct{0};   // 0..100
  int volatility_pct{0};  // 0..100
  int hazard_pct{0};      // 0..100
  bool spoof_risk{false};
};

inline AnomalyScanReadout anomaly_scan_readout(const Anomaly& a,
                                               double nebula_density,
                                               double ruins_density,
                                               double pirate_risk_effective) {
  const double neb = std::clamp(nebula_density, 0.0, 1.0);
  const double ruins = std::clamp(ruins_density, 0.0, 1.0);
  const double pir = std::clamp(pirate_risk_effective, 0.0, 1.0);

  const std::uint64_t s = anomaly_seed(a) ^ 0x7C4A3BD152E6F901ULL;
  auto urand = [&](std::uint64_t salt) {
    return u01_from_u64(splitmix64(s ^ salt));
  };

  double coherence = 55.0 + (urand(0x1111111111111111ULL) - 0.5) * 30.0;
  double volatility = 32.0 + (urand(0x2222222222222222ULL) - 0.5) * 36.0;
  double hazard = 20.0 + (urand(0x3333333333333333ULL) - 0.5) * 22.0;

  switch (a.kind) {
    case AnomalyKind::Ruins:
    case AnomalyKind::Artifact:
      coherence += 12.0 + ruins * 18.0;
      volatility += 3.0 + neb * 5.0;
      hazard += 8.0;
      break;
    case AnomalyKind::Xenoarchaeology:
      coherence += 14.0 + ruins * 16.0;
      volatility += 8.0 + neb * 9.0;
      hazard += 14.0;
      break;
    case AnomalyKind::Distress:
      coherence -= 4.0 + pir * 20.0;
      volatility += 14.0 + pir * 18.0;
      hazard += 10.0 + pir * 22.0;
      break;
    case AnomalyKind::Phenomenon:
      coherence -= 6.0 + neb * 16.0;
      volatility += 22.0 + neb * 20.0;
      hazard += 16.0 + neb * 18.0;
      break;
    case AnomalyKind::Distortion:
      coherence -= 12.0 + neb * 18.0;
      volatility += 30.0 + neb * 22.0;
      hazard += 22.0 + pir * 12.0;
      break;
    case AnomalyKind::Signal:
    default:
      coherence += 3.0 - neb * 5.0;
      volatility += 9.0 + neb * 10.0;
      hazard += 6.0;
      break;
  }

  coherence = std::clamp(coherence, 2.0, 98.0);
  volatility = std::clamp(volatility, 1.0, 99.0);
  hazard += pir * 24.0 + neb * 10.0 - ruins * 6.0 + volatility * 0.20 - coherence * 0.08;
  hazard = std::clamp(hazard, 1.0, 99.0);

  AnomalyScanReadout out;
  out.coherence_pct = static_cast<int>(std::lround(coherence));
  out.volatility_pct = static_cast<int>(std::lround(volatility));
  out.hazard_pct = static_cast<int>(std::lround(hazard));
  out.focus_domain = anomaly_theme_domain(a);
  out.spoof_risk =
      (a.kind == AnomalyKind::Distress && (pir > 0.35 || out.volatility_pct >= 62)) ||
      (a.kind == AnomalyKind::Signal && pir > 0.65 && out.coherence_pct < 45);

  if (out.coherence_pct >= 72 && out.volatility_pct <= 35) {
    out.resonance = AnomalyResonanceBand::Harmonic;
  } else if (out.volatility_pct >= 78) {
    out.resonance = AnomalyResonanceBand::Chaotic;
  } else if (out.coherence_pct <= 28) {
    out.resonance = AnomalyResonanceBand::NullLocked;
  } else if (out.volatility_pct >= 55) {
    out.resonance = AnomalyResonanceBand::Fractured;
  } else {
    out.resonance = AnomalyResonanceBand::Quiescent;
  }

  return out;
}

inline std::string anomaly_scan_brief(const AnomalyScanReadout& r) {
  std::string out;
  out.reserve(96);
  out += anomaly_resonance_band_label(r.resonance);
  out += " / ";
  out += theme_domain_label(r.focus_domain);
  out += " | C";
  out += std::to_string(r.coherence_pct);
  out += " V";
  out += std::to_string(r.volatility_pct);
  out += " H";
  out += std::to_string(r.hazard_pct);
  if (r.spoof_risk) out += " | spoof-risk";
  return out;
}

inline std::string anomaly_scan_brief(const Anomaly& a,
                                      double nebula_density,
                                      double ruins_density,
                                      double pirate_risk_effective) {
  return anomaly_scan_brief(anomaly_scan_readout(a, nebula_density, ruins_density, pirate_risk_effective));
}

// --- anomaly site profiles (procedural risk/reward archetypes) ------------------
//
// A deterministic "site profile" adds gameplay variation without new save fields.
// It is generated from anomaly identity + local environment and can tune
// investigation depth, reward pressure and hazard pressure in a coherent way.
enum class AnomalySiteArchetype : std::uint8_t {
  QuietDrift = 0,
  SignalLattice = 1,
  RelicVault = 2,
  FractureNest = 3,
  TurbulencePocket = 4,
  DecoyWeb = 5,
};

inline const char* anomaly_site_archetype_label(AnomalySiteArchetype a) {
  switch (a) {
    case AnomalySiteArchetype::QuietDrift: return "Quiet Drift";
    case AnomalySiteArchetype::SignalLattice: return "Signal Lattice";
    case AnomalySiteArchetype::RelicVault: return "Relic Vault";
    case AnomalySiteArchetype::FractureNest: return "Fracture Nest";
    case AnomalySiteArchetype::TurbulencePocket: return "Turbulence Pocket";
    case AnomalySiteArchetype::DecoyWeb: return "Decoy Web";
    default: return "Unknown";
  }
}

struct AnomalySiteProfile {
  AnomalySiteArchetype archetype{AnomalySiteArchetype::QuietDrift};
  double investigation_mult{1.0};
  int investigation_add_days{0};
  double research_mult{1.0};
  double mineral_mult{1.0};
  double hazard_chance_mult{1.0};
  double hazard_damage_mult{1.0};
  double unlock_bonus{0.0};
  double cache_bonus{0.0};
};

inline std::uint64_t anomaly_env_seed_bits(double nebula_density,
                                           double ruins_density,
                                           double pirate_risk_effective,
                                           double gradient01) {
  const auto q01 = [](double v, int max_q) -> std::uint64_t {
    const double x = std::clamp(v, 0.0, 1.0);
    const int q = static_cast<int>(std::lround(x * static_cast<double>(std::max(1, max_q))));
    return static_cast<std::uint64_t>(std::clamp(q, 0, std::max(1, max_q)));
  };

  std::uint64_t h = 0x6A09E667F3BCC909ULL;
  h ^= q01(nebula_density, 1023) * 0x9E3779B97F4A7C15ULL;
  h ^= q01(ruins_density, 1023) * 0xD6E8FEB86659FD93ULL;
  h ^= q01(pirate_risk_effective, 1023) * 0x94D049BB133111EBULL;
  h ^= q01(gradient01, 1023) * 0xA24BAED4963EE407ULL;
  return splitmix64(h);
}

inline AnomalySiteProfile anomaly_site_profile(const Anomaly& a,
                                               double nebula_density,
                                               double ruins_density,
                                               double pirate_risk_effective,
                                               double gradient01) {
  const double neb = std::clamp(nebula_density, 0.0, 1.0);
  const double ruins = std::clamp(ruins_density, 0.0, 1.0);
  const double pir = std::clamp(pirate_risk_effective, 0.0, 1.0);
  const double grad = std::clamp(gradient01, 0.0, 1.0);

  const std::uint64_t seed = splitmix64(anomaly_seed(a) ^ anomaly_env_seed_bits(neb, ruins, pir, grad) ^
                                        0xF1357AEA2E62A9C5ULL);
  auto urand = [&](std::uint64_t salt) { return u01_from_u64(splitmix64(seed ^ salt)); };

  const double is_signal = (a.kind == AnomalyKind::Signal || a.kind == AnomalyKind::Echo) ? 1.0 : 0.0;
  const double is_distress = (a.kind == AnomalyKind::Distress) ? 1.0 : 0.0;
  const double is_ruin =
      (a.kind == AnomalyKind::Ruins || a.kind == AnomalyKind::Artifact || a.kind == AnomalyKind::Xenoarchaeology)
          ? 1.0
          : 0.0;
  const double is_turb = (a.kind == AnomalyKind::Phenomenon || a.kind == AnomalyKind::Distortion) ? 1.0 : 0.0;

  // Weighted archetype selection.
  double w_quiet = 0.35 + 0.55 * (1.0 - neb) * (1.0 - grad) + 0.15 * (1.0 - pir);
  double w_signal = 0.20 + 0.90 * is_signal + 0.45 * (1.0 - neb) + 0.20 * (1.0 - pir);
  double w_relic = 0.20 + 1.10 * is_ruin + 1.05 * ruins + 0.15 * (1.0 - pir);
  double w_fracture = 0.14 + 0.90 * grad + 0.50 * is_turb + 0.20 * ruins;
  double w_turb = 0.12 + 0.85 * neb + 0.62 * is_turb + 0.35 * grad;
  double w_decoy = 0.08 + 0.95 * pir + 0.50 * is_distress + 0.30 * is_signal;

  w_quiet = std::max(0.01, w_quiet);
  w_signal = std::max(0.01, w_signal);
  w_relic = std::max(0.01, w_relic);
  w_fracture = std::max(0.01, w_fracture);
  w_turb = std::max(0.01, w_turb);
  w_decoy = std::max(0.01, w_decoy);

  const double wsum = w_quiet + w_signal + w_relic + w_fracture + w_turb + w_decoy;
  const double u = urand(0x1111111111111111ULL) * wsum;

  AnomalySiteProfile out;
  if (u < w_quiet) {
    out.archetype = AnomalySiteArchetype::QuietDrift;
    out.investigation_mult = 0.93;
    out.investigation_add_days = -1;
    out.research_mult = 0.96;
    out.mineral_mult = 0.92;
    out.hazard_chance_mult = 0.72;
    out.hazard_damage_mult = 0.78;
    out.unlock_bonus = 0.02;
    out.cache_bonus = -0.03;
  } else if (u < w_quiet + w_signal) {
    out.archetype = AnomalySiteArchetype::SignalLattice;
    out.investigation_mult = 0.99;
    out.investigation_add_days = 0;
    out.research_mult = 1.10;
    out.mineral_mult = 0.96;
    out.hazard_chance_mult = 0.95;
    out.hazard_damage_mult = 0.95;
    out.unlock_bonus = 0.09;
    out.cache_bonus = -0.01;
  } else if (u < w_quiet + w_signal + w_relic) {
    out.archetype = AnomalySiteArchetype::RelicVault;
    out.investigation_mult = 1.10;
    out.investigation_add_days = 2;
    out.research_mult = 1.17;
    out.mineral_mult = 1.22;
    out.hazard_chance_mult = 1.05;
    out.hazard_damage_mult = 1.06;
    out.unlock_bonus = 0.07;
    out.cache_bonus = 0.09;
  } else if (u < w_quiet + w_signal + w_relic + w_fracture) {
    out.archetype = AnomalySiteArchetype::FractureNest;
    out.investigation_mult = 1.05;
    out.investigation_add_days = 1;
    out.research_mult = 1.12;
    out.mineral_mult = 1.08;
    out.hazard_chance_mult = 1.22;
    out.hazard_damage_mult = 1.18;
    out.unlock_bonus = 0.02;
    out.cache_bonus = 0.03;
  } else if (u < w_quiet + w_signal + w_relic + w_fracture + w_turb) {
    out.archetype = AnomalySiteArchetype::TurbulencePocket;
    out.investigation_mult = 1.03;
    out.investigation_add_days = 0;
    out.research_mult = 1.06;
    out.mineral_mult = 1.04;
    out.hazard_chance_mult = 1.26;
    out.hazard_damage_mult = 1.20;
    out.unlock_bonus = 0.01;
    out.cache_bonus = 0.03;
  } else {
    out.archetype = AnomalySiteArchetype::DecoyWeb;
    out.investigation_mult = 0.98;
    out.investigation_add_days = 0;
    out.research_mult = 0.93;
    out.mineral_mult = 0.90;
    out.hazard_chance_mult = 1.20;
    out.hazard_damage_mult = 1.08;
    out.unlock_bonus = -0.04;
    out.cache_bonus = -0.02;
  }

  // Small deterministic jitter so profiles are not perfectly discrete buckets.
  const double jitter = (urand(0x2222222222222222ULL) - 0.5) * 0.08;
  const double jitter_h = (urand(0x3333333333333333ULL) - 0.5) * 0.10;
  out.investigation_mult = std::clamp(out.investigation_mult * (1.0 + jitter), 0.80, 1.35);
  out.research_mult = std::clamp(out.research_mult * (1.0 + jitter), 0.75, 1.50);
  out.mineral_mult = std::clamp(out.mineral_mult * (1.0 + jitter * 0.9), 0.70, 1.60);
  out.hazard_chance_mult = std::clamp(out.hazard_chance_mult * (1.0 + jitter_h), 0.50, 1.75);
  out.hazard_damage_mult = std::clamp(out.hazard_damage_mult * (1.0 + jitter_h), 0.60, 1.85);

  // Kind-local nudges.
  if (a.kind == AnomalyKind::Xenoarchaeology || a.kind == AnomalyKind::Artifact) {
    out.research_mult = std::clamp(out.research_mult + 0.04, 0.75, 1.50);
    out.mineral_mult = std::clamp(out.mineral_mult + 0.06, 0.70, 1.60);
  }
  if (a.kind == AnomalyKind::Distress && out.archetype == AnomalySiteArchetype::DecoyWeb) {
    out.hazard_chance_mult = std::clamp(out.hazard_chance_mult + 0.12, 0.50, 1.75);
  }

  return out;
}

inline std::string anomaly_site_profile_brief(const AnomalySiteProfile& p) {
  std::string out;
  out.reserve(72);
  out += anomaly_site_archetype_label(p.archetype);
  out += " | RPx";
  out += std::to_string(static_cast<int>(std::lround(p.research_mult * 100.0)));
  out += "% Hx";
  out += std::to_string(static_cast<int>(std::lround(p.hazard_chance_mult * 100.0)));
  out += "%";
  return out;
}

inline std::string anomaly_site_profile_brief(const Anomaly& a,
                                              double nebula_density,
                                              double ruins_density,
                                              double pirate_risk_effective,
                                              double gradient01) {
  return anomaly_site_profile_brief(
      anomaly_site_profile(a, nebula_density, ruins_density, pirate_risk_effective, gradient01));
}

// --- convergence weave (emergent cross-system between procgen layers) ----------
//
// Convergence Weave combines:
// - scan readout (coherence/volatility/hazard),
// - site archetype (risk/reward archetypes),
// - local environmental pressure (nebula/ruins/pirate/gradient),
// to determine how strongly a newly generated anomaly should "snap" into an
// existing local anomaly chain.
struct AnomalyConvergenceProfile {
  double link_chance{0.0};        // chance to attach to a nearby unresolved site
  double link_radius_mkm{36.0};   // search radius for potential parent anomalies
  int extra_investigation_days{0};
  double research_mult{1.0};
  double mineral_mult{1.0};
  double hazard_mult{1.0};
  double cache_bonus{0.0};        // additive cache spawn chance bonus
};

inline AnomalyConvergenceProfile anomaly_convergence_profile(const Anomaly& a,
                                                             const AnomalyScanReadout& scan,
                                                             const AnomalySiteProfile& site,
                                                             double nebula_density,
                                                             double ruins_density,
                                                             double pirate_risk_effective,
                                                             double gradient01) {
  const double neb = std::clamp(nebula_density, 0.0, 1.0);
  const double ruins = std::clamp(ruins_density, 0.0, 1.0);
  const double pir = std::clamp(pirate_risk_effective, 0.0, 1.0);
  const double grad = std::clamp(gradient01, 0.0, 1.0);

  double link = 0.04 + 0.12 * ruins + 0.10 * neb + 0.10 * grad + 0.06 * pir;
  double radius = 28.0 + 44.0 * grad + 18.0 * ruins + 10.0 * neb;
  double rp_mult = 1.00 + 0.0012 * static_cast<double>(scan.coherence_pct) + 0.08 * ruins;
  double mineral_mult = 1.00 + 0.15 * ruins + 0.05 * neb;
  double hazard_mult = 1.00 + 0.0018 * static_cast<double>(scan.volatility_pct) + 0.12 * grad;
  double cache_bonus = 0.02 + 0.10 * ruins + 0.04 * neb;
  int extra_days = (scan.volatility_pct >= 62) ? 1 : 0;

  switch (scan.resonance) {
    case AnomalyResonanceBand::Harmonic:
      link += 0.08;
      radius += 10.0;
      rp_mult += 0.06;
      break;
    case AnomalyResonanceBand::Fractured:
      link += 0.10;
      radius += 14.0;
      rp_mult += 0.08;
      hazard_mult += 0.12;
      extra_days += 1;
      break;
    case AnomalyResonanceBand::Chaotic:
      link += 0.16;
      radius += 18.0;
      rp_mult += 0.10;
      hazard_mult += 0.24;
      extra_days += 1;
      break;
    case AnomalyResonanceBand::NullLocked:
      link += 0.05;
      radius += 6.0;
      rp_mult -= 0.04;
      break;
    case AnomalyResonanceBand::Quiescent:
    default:
      break;
  }

  switch (site.archetype) {
    case AnomalySiteArchetype::RelicVault:
      link += 0.08;
      rp_mult += 0.07;
      mineral_mult += 0.16;
      cache_bonus += 0.09;
      extra_days += 1;
      break;
    case AnomalySiteArchetype::FractureNest:
      link += 0.10;
      hazard_mult += 0.16;
      rp_mult += 0.05;
      extra_days += 1;
      break;
    case AnomalySiteArchetype::TurbulencePocket:
      link += 0.07;
      hazard_mult += 0.12;
      break;
    case AnomalySiteArchetype::SignalLattice:
      link += 0.06;
      rp_mult += 0.05;
      break;
    case AnomalySiteArchetype::DecoyWeb:
      link += 0.04;
      hazard_mult += 0.10;
      rp_mult -= 0.03;
      cache_bonus -= 0.03;
      break;
    case AnomalySiteArchetype::QuietDrift:
    default:
      break;
  }

  if (a.kind == AnomalyKind::Distortion || a.kind == AnomalyKind::Phenomenon) {
    link += 0.07;
    hazard_mult += 0.14;
  } else if (a.kind == AnomalyKind::Xenoarchaeology || a.kind == AnomalyKind::Artifact) {
    link += 0.09;
    rp_mult += 0.08;
    mineral_mult += 0.12;
  } else if (a.kind == AnomalyKind::Distress && scan.spoof_risk) {
    link += 0.03;
    hazard_mult += 0.10;
    cache_bonus -= 0.04;
  }

  // Blend in site-level multipliers so the weave reacts to the existing profile.
  rp_mult *= std::clamp(site.research_mult, 0.70, 1.60);
  mineral_mult *= std::clamp(site.mineral_mult, 0.70, 1.70);
  hazard_mult *= std::clamp(site.hazard_chance_mult * 0.5 + site.hazard_damage_mult * 0.5, 0.60, 1.90);

  AnomalyConvergenceProfile out;
  out.link_chance = std::clamp(link, 0.0, 0.88);
  out.link_radius_mkm = std::clamp(radius, 20.0, 140.0);
  out.extra_investigation_days = std::clamp(extra_days, 0, 4);
  out.research_mult = std::clamp(rp_mult, 0.70, 1.90);
  out.mineral_mult = std::clamp(mineral_mult, 0.65, 2.10);
  out.hazard_mult = std::clamp(hazard_mult, 0.70, 2.30);
  out.cache_bonus = std::clamp(cache_bonus, -0.20, 0.35);
  return out;
}

inline std::string generate_anomaly_name(const Anomaly& a) {
  const std::uint64_t s = anomaly_seed(a);
  HashRng rng(s ^ 0x1B03738712F44E3DULL);
  const std::string theme = anomaly_theme_label(a);

  // Kind-specific node vocabulary.
  static constexpr std::array<const char*, 18> kRuins = {
      "Obelisk",     "Archive Node", "Sealed Hall",   "Sunken Atrium", "Vault Door",
      "Broken Gate", "Reliquary",    "Glyph Court",   "Silent Annex", "Spiral Stair",
      "Data Choir",  "Basalt Lens",  "Cenotaph",      "Buried Spire", "Resonator",
      "Shard Chapel", "Mirror Ossuary", "Foundry Ring",
  };
  static constexpr std::array<const char*, 18> kSignal = {
      "Whisper",      "Carrier Echo", "Harmonic Knot", "Pulsed Chorus", "Cold Beacon",
      "Ghost Packet", "Needleband",   "Drift Tone",    "Lingerwave",    "Lattice Ping",
      "Phase Murmur", "Long Call",    "Broken Cadence", "Quiet Loop",   "Siren Fragment",
      "Index Tone",   "Cipher Bloom", "Aural Trace",
  };
  static constexpr std::array<const char*, 18> kPhenom = {
      "Shear Point",    "Lensing Bloom", "Ion Veil",      "Gravity Scar",  "Vector Fold",
      "Eddy Crown",     "Tidal Knot",    "Spacetime Ripple","Dust Halo",    "Arc Pocket",
      "Refraction Cone", "Soft Singularity", "Magnetic Sleet", "Null Wake", "Chiral Wake",
      "Spectral Tear",  "Plasma Lace",  "Phase Reef",
  };
  static constexpr std::array<const char*, 12> kDistress = {
      "Beacon", "Mayday", "SOS", "Lifepod Ping", "Blackbox", "Emergency Burst",
      "Rescue Code", "Distress Loop", "Wreck Ping", "Autopilot Plea", "Hull Tap", "Last Call",
  };
  static constexpr std::array<const char*, 12> kDistortion = {
      "Shear Gate",   "Null Choir",    "Curvature Knot",  "Warped Mirror", "Fissure Choir", "Fractured Lens",
      "Bent Halo",    "Temporal Fold", "Signal Scar",     "Gravity Veil",  "Phase Fold",    "Clockwork Rift",
  };
  static constexpr std::array<const char*, 12> kXeno = {
      "Silent Vault", "Buried Temple", "Precursor Spire", "Ancestral Engine", "Monolith Choir",
      "Obscure Archive", "Shard Shrine", "Lost Reliquary", "Glyph Tomb", "Stellar Mosaics",
      "Cold Vault", "Void Catacomb",
  };

  const int fmt = rng.range_int(0, 3);

  auto make_tag = [&]() {
    const std::string code = anomaly_signature_code(a);
    // Keep tag compact.
    return std::string("[") + code + "]";
  };

  if (a.kind == AnomalyKind::Distress) {
    const char* head = pick_from(kDistress, rng);
    // Callsign-like suffix.
    const std::string call = hex_n(rng.next_u64() ^ s, 4);
    if (fmt == 0) return std::string(head) + " " + call;
    if (fmt == 1) return std::string(head) + " " + call + " " + make_tag();
    return std::string(head) + ": " + call;
  }

  const bool is_ruins = (a.kind == AnomalyKind::Ruins || a.kind == AnomalyKind::Artifact);
  const bool is_phen = (a.kind == AnomalyKind::Phenomenon);
  const bool is_dist = (a.kind == AnomalyKind::Distortion);
  const bool is_xeno = (a.kind == AnomalyKind::Xenoarchaeology);
  const char* node = nullptr;
  if (is_dist) node = pick_from(kDistortion, rng);
  else if (is_ruins) node = pick_from(kRuins, rng);
  else if (is_phen) node = pick_from(kPhenom, rng);
  else if (is_xeno) node = pick_from(kXeno, rng);
  else node = pick_from(kSignal, rng);

  // A few formats so lists don't look like clones.
  if (fmt == 0) {
    return theme + ": " + node;
  }
  if (fmt == 1) {
    return std::string(node) + " of " + theme;
  }
  if (fmt == 2) {
    return theme + " " + node;
  }
  return theme + ": " + node + " " + make_tag();
}

inline std::string generate_wreck_cache_name(const Wreck& w, std::string_view tag = {}) {
  const std::uint64_t s = wreck_seed(w, tag);
  HashRng rng(s ^ 0xDB4F0B9175AE2165ULL);

  static constexpr std::array<const char*, 14> kNouns = {
      "Cache", "Stash", "Locker", "Crate", "Strongbox", "Hold",
      "Pod", "Drift Vault", "Pallet", "Sealed Drum", "Cargo Coffin", "Jettison Box",
      "Hidden Bay", "Cold Safe",
  };
  static constexpr std::array<const char*, 10> kAdjs = {
      "Drifting", "Quiet", "Scorched", "Sealed", "Salted", "Frosted", "Black", "Silted", "Brass", "Nameless",
  };

  std::string prefix;
  if (!tag.empty()) {
    prefix = std::string(tag);
  } else {
    prefix = std::string(pick_from(kAdjs, rng));
  }

  const std::string sig = wreck_signature_code(w, tag);
  const char* noun = pick_from(kNouns, rng);

  const int fmt = rng.range_int(0, 2);
  if (fmt == 0) return prefix + " " + noun;
  if (fmt == 1) return prefix + " " + noun + " [" + sig + "]";
  return std::string(noun) + " " + prefix + " [" + sig + "]";
}

inline std::string anomaly_lore_line(const Anomaly& a,
                                    double nebula_density,
                                    double ruins_density,
                                    double pirate_risk_effective) {
  const std::uint64_t s = anomaly_seed(a);
  HashRng rng(s ^ 0x94D049BB133111EBULL);
  const std::string theme = anomaly_theme_label(a);
  const std::string sig = anomaly_signature_code(a);

  const double neb = std::clamp(nebula_density, 0.0, 1.0);
  const double ruins = std::clamp(ruins_density, 0.0, 1.0);
  const double pir = std::clamp(pirate_risk_effective, 0.0, 1.0);

  // A handful of short fragments; we stitch them together based on kind.
  static constexpr std::array<const char*, 12> kRuinsA = {
      "Layered alloys", "Dormant emitters", "Fractured glyph panels", "Pressure-sealed bulkheads",
      "An ossified sensor mast", "A ring of broken conduits", "A sealed maintenance hatch", "A collapsed atrium",
      "A brittle ceramic lattice", "A field of cracked pylons", "A lightless corridor", "A scorched vault door",
  };
  static constexpr std::array<const char*, 12> kRuinsB = {
      "pre-date local formation", "resist spectrographic classification", "hum below the noise floor", "respond to narrowband pings",
      "emit a faint thermal afterimage", "appear self-repairing", "carry non-human indexing marks", "show recursive machining",
      "contain vacuum-cast voids", "interlock without fasteners", "vibrate under thrust", "mirror the system's magnetic field",
  };
  static constexpr std::array<const char*, 12> kSignalA = {
      "A narrowband carrier", "A broken handshake", "A repeating chirp", "A phased whisper",
      "A coded chorus", "A cold beacon", "A fragmentary burst", "A drifting tone",
      "A prismatic ping", "A punctured cadence", "A braided packet", "A subharmonic call",
  };
  static constexpr std::array<const char*, 12> kSignalB = {
      "slides between harmonics", "folds back on itself", "stutters in prime intervals", "arrives slightly out of phase",
      "repeats every few minutes", "inverts when boosted", "splits across sensor bands", "vanishes at close range",
      "locks onto drive emissions", "carries a corrupted registry", "echoes from multiple vectors", "flares during storm peaks",
  };
  static constexpr std::array<const char*, 12> kPhenomA = {
      "Local spacetime", "The dust field", "An ion veil", "A lensing bloom",
      "A gravity eddy", "Magnetic sleet", "A refraction cone", "A chiral wake",
      "A phase reef", "A tidal knot", "A plasma lace", "A null wake",
  };
  static constexpr std::array<const char*, 12> kPhenomB = {
      "shows shear and shimmering", "distorts range returns", "drains shields slowly", "compresses sensor horizons",
      "produces ghost contacts", "bends laser ranging", "scrambles active sweeps", "flares under thrust",
      "creates false parallax", "hides cold bodies", "magnifies heat plumes", "warps intercept solutions",
  };
  static constexpr std::array<const char*, 10> kDistA = {
      "An automated beacon", "A cracked blackbox", "A rescue transponder", "A degraded IFF",
      "A hull-tap pattern", "A lifepod ping", "A distress loop", "An emergency burst",
      "A panicked registry", "A sputtering mayday",
  };
  static constexpr std::array<const char*, 10> kDistB = {
      "repeats an incomplete call", "interleaves unknown tones", "cycles broken coordinates", "broadcasts from multiple points",
      "changes when approached", "drops packets in nebula haze", "carries spoofed timestamps", "matches pirate bait profiles",
      "disagrees with stellar ephemeris", "collides with sensor ghosts",
  };
  static constexpr std::array<const char*, 12> kDistortionA = {
      "A local spacetime seam", "A null-locked wake", "An unstable warp braid", "A curved lens path",
      "A drifting harmonic knot", "A warped sensor horizon", "A fractured horizon line", "A filament-shredded pocket",
      "A gravitational pinch", "A phase-shifted cloud", "A silent fold", "A mirrored beacon echo",
  };
  static constexpr std::array<const char*, 12> kDistortionB = {
      "shifts position when not observed", "inverts microfield gradients", "distorts range with every approach",
      "hums at half the expected frequency", "shows impossible parallax", "bends dust flow into rings",
      "changes under repeated scanning", "synchronizes with high-throttle maneuvers", "drifts without inertia",
      "scrambles long-baseline telemetry", "flickers between frames", "reacts to gravitic load",
  };
  static constexpr std::array<const char*, 12> kXenoA = {
      "An intact alloy gate", "An old survey beacon", "A buried transit vault", "A sealed reactor chamber",
      "A fractured star-map", "A ceremonial datacore", "A buried sensor lattice", "A long-dead habitat ring",
      "A relic of synthetic architecture", "A fossilized jump anchor", "A silent maintenance spine", "A buried field tower",
  };
  static constexpr std::array<const char*, 12> kXenoB = {
      "still runs predictive maintenance", "hides layered indexing marks", "stores preserved civic records",
      "carries non-human fabrication marks", "matches no known standards", "reacts to cargo drones",
      "contains sealed specimen racks", "shows periodic thermal pulses", "responds to synchronized scans",
      "emits a low static harmonic", "appears to predate the cluster", "maps into recursive vectors",
  };

  std::string line;
  line.reserve(160);

  // Kind-specific sentence.
  if (a.kind == AnomalyKind::Ruins || a.kind == AnomalyKind::Artifact) {
    line = std::string(pick_from(kRuinsA, rng)) + " " + pick_from(kRuinsB, rng);
    if (ruins > 0.55) line += "; the site feels intentionally concealed";
  } else if (a.kind == AnomalyKind::Phenomenon) {
    line = std::string(pick_from(kPhenomA, rng)) + " " + pick_from(kPhenomB, rng);
    if (neb > 0.60) line += "; nebular ions amplify the effect";
  } else if (a.kind == AnomalyKind::Distress) {
    line = std::string(pick_from(kDistA, rng)) + " " + pick_from(kDistB, rng);
    if (pir > 0.55) line += "; analysts warn of pirate spoofing";
  } else if (a.kind == AnomalyKind::Distortion) {
    line = std::string(pick_from(kDistortionA, rng)) + " " + pick_from(kDistortionB, rng);
    if (neb > 0.65) line += "; distortion effects are strongest across dense dust filaments";
    if (pir > 0.45) line += "; navigational data drift suggests remote interference";
  } else if (a.kind == AnomalyKind::Xenoarchaeology) {
    line = std::string(pick_from(kXenoA, rng)) + " " + pick_from(kXenoB, rng);
    if (pir > 0.20) line += "; non-localized thermal drift complicates extraction";
    if (ruins > 0.55) line += "; relic architecture looks intentionally hidden";
  } else {
    line = std::string(pick_from(kSignalA, rng)) + " " + pick_from(kSignalB, rng);
    if (neb > 0.50) line += "; signal is smeared by nebula haze";
  }

  // Theme tag (tiny) + signature.
  if (!theme.empty()) {
    line += ". Theme tag: " + theme;
  }
  const AnomalyScanReadout scan = anomaly_scan_readout(a, neb, ruins, pir);
  line += ". Scan profile: ";
  line += anomaly_resonance_band_label(scan.resonance);
  line += " / ";
  line += theme_domain_label(scan.focus_domain);
  line += " (C";
  line += std::to_string(scan.coherence_pct);
  line += ", V";
  line += std::to_string(scan.volatility_pct);
  line += ", H";
  line += std::to_string(scan.hazard_pct);
  if (scan.spoof_risk) {
    line += ", spoof risk elevated";
  }
  line += ")";
  line += ". Signature: " + sig + ".";
  return line;
}


// --- codex fragments (ciphered lore) ------------------------------------------
//
// A codex fragment is a deterministic short message paired with a monoalphabetic
// substitution cipher. The plaintext can be gradually revealed by masking
// characters based on a decode fraction.
//
// This is designed to support "soft progression" in the UI: as a faction
// resolves more anomalies in the same lead-chain, the translation becomes less
// garbled without storing any extra per-anomaly state.

inline std::string to_upper_ascii(std::string s) {
  for (char& c : s) {
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
  }
  return s;
}

inline std::array<char, 26> monoalpha_cipher_map(std::uint64_t seed) {
  std::array<char, 26> map{};
  for (int i = 0; i < 26; ++i) map[static_cast<std::size_t>(i)] = static_cast<char>('A' + i);

  HashRng rng(splitmix64(seed ^ 0xC0DEC0DEF00DFACEULL));
  for (int i = 25; i > 0; --i) {
    const int j = rng.range_int(0, i);
    std::swap(map[static_cast<std::size_t>(i)], map[static_cast<std::size_t>(j)]);
  }
  return map;
}

inline char monoalpha_apply(char c, const std::array<char, 26>& map) {
  if (c >= 'A' && c <= 'Z') return map[static_cast<std::size_t>(c - 'A')];
  if (c >= 'a' && c <= 'z') {
    const char up = map[static_cast<std::size_t>(c - 'a')];
    return static_cast<char>(up - 'A' + 'a');
  }
  return c;
}

inline std::string monoalpha_encode(std::string_view text, std::uint64_t seed) {
  const auto map = monoalpha_cipher_map(seed);
  std::string out;
  out.resize(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    out[i] = monoalpha_apply(text[i], map);
  }
  return out;
}

inline std::string codex_plaintext(const Anomaly& a) {
  std::uint64_t s = anomaly_seed(a) ^ 0x9E3779B97F4A7C15ULL;
  HashRng rng(splitmix64(s));

  std::string theme = to_upper_ascii(anomaly_theme_label(a));
  const ThemeDomain dom = anomaly_theme_domain(a);
  std::string dom_label = to_upper_ascii(theme_domain_label(dom));

  static constexpr std::array<const char*, 14> kVerbs = {
      "REMEMBERS", "GUARDS", "CATALOGS", "OBSCURES", "SINGS TO", "BRAIDS", "HIDES",
      "MEASURES", "ECHOES", "FOLDS", "ANNOTATES", "SEALS", "REVERSES", "FORECASTS",
  };
  static constexpr std::array<const char*, 18> kTargets = {
      "THE BEACON", "THE VAULT", "THE LENS", "THE VECTOR", "THE KEY", "THE GATE",
      "THE ORCHARD", "THE INDEX", "THE RELIQUARY", "THE CHOIR", "THE CIRCUIT", "THE ARCHIVE",
      "THE COMPASS", "THE MERIDIAN", "THE APERTURE", "THE WOUND", "THE LANTERN", "THE RESERVOIR",
  };
  static constexpr std::array<const char*, 12> kImperatives = {
      "TRACE", "FOLLOW", "ALIGN", "LISTEN", "CALIBRATE", "DESCEND", "ASCEND",
      "INVERT", "WAIT", "BURN", "MEASURE", "REFUSE",
  };
  static constexpr std::array<const char*, 10> kDirections = {
      "COREWARD", "RIMWARD", "SPINWARD", "ANTISPINWARD", "INWARD", "OUTWARD",
      "ZENITH", "NADIR", "ALONG THE DUST", "AGAINST THE STORM",
  };
  static constexpr std::array<const char*, 10> kQualifiers = {
      "AT LOW POWER", "UNDER EMCON", "WHEN THE NEBULA THINS", "ON THE THIRD PASS",
      "BETWEEN PULSES", "AFTER THE FIRST SILENCE", "BEFORE IMPACT", "WHEN THE CLOCK STUTTERS",
      "DURING ECLIPSE", "IN NEGATIVE TIME",
  };

  const char* verb = pick_from(kVerbs, rng);
  const char* target = pick_from(kTargets, rng);
  const char* imp = pick_from(kImperatives, rng);
  const char* dir = pick_from(kDirections, rng);
  const char* qual = pick_from(kQualifiers, rng);

  const std::string seal_a = hex_n(rng.next_u64(), 4);
  const std::string seal_b = hex_n(rng.next_u64(), 4);

  std::string msg;
  msg.reserve(200);
  msg += "THE ";
  msg += theme;
  msg += " ";
  msg += verb;
  msg += " ";
  msg += target;
  msg += ". ";
  msg += imp;
  msg += " ";
  msg += dir;
  msg += " ";
  msg += qual;
  msg += ". ";
  msg += "DOMAIN: ";
  msg += dom_label;
  msg += ". ";
  msg += "SEAL ";
  msg += seal_a;
  msg += "-";
  msg += seal_b;
  msg += ".";
  return msg;
}

inline std::string codex_ciphertext(const Anomaly& a) {
  const std::string plain = codex_plaintext(a);
  const std::uint64_t seed = anomaly_seed(a) ^ 0xD1B54A32D192ED03ULL;
  return monoalpha_encode(plain, seed);
}

inline std::string codex_partial_plaintext(const Anomaly& a, double decode_fraction) {
  const double f = std::clamp(decode_fraction, 0.0, 1.0);
  const std::string plain = codex_plaintext(a);
  if (f >= 0.999) return plain;

  std::string out = plain;
  const std::uint64_t seed = anomaly_seed(a) ^ 0xA5A5A5A5D00DF00DULL;
  for (std::size_t i = 0; i < out.size(); ++i) {
    const char c = out[i];
    const bool is_alnum = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
    if (!is_alnum) continue;

    const std::uint64_t h = splitmix64(seed ^ (static_cast<std::uint64_t>(i) * 0x9e3779b97f4a7c15ULL));
    const double u = u01_from_u64(h);
    if (u > f) out[i] = '.';
  }
  return out;
}

}  // namespace nebula4x::procgen_obscure
