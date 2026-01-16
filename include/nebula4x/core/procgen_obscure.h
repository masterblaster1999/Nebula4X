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
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

#include "nebula4x/core/entities.h"
#include "nebula4x/core/ids.h"

namespace nebula4x::procgen_obscure {

// --- low-level deterministic mixing / RNG -------------------------------------

// splitmix64: fast deterministic mixing suitable for procedural noise.
// Derived from the reference implementation by Sebastiano Vigna.
inline std::uint64_t splitmix64(std::uint64_t x) {
  x += 0x9e3779b97f4a7c15ULL;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
  return x ^ (x >> 31);
}

inline double u01_from_u64(std::uint64_t x) {
  // 53-bit mantissa for IEEE-754 double.
  const std::uint64_t v = x >> 11;
  return static_cast<double>(v) * (1.0 / 9007199254740992.0);  // 2^53
}

struct HashRng {
  std::uint64_t s{0};

  explicit HashRng(std::uint64_t seed) : s(seed) {}

  std::uint64_t next_u64() {
    s = splitmix64(s);
    return s;
  }

  double next_u01() { return u01_from_u64(next_u64()); }

  int range_int(int lo_incl, int hi_incl) {
    int lo = lo_incl;
    int hi = hi_incl;
    if (hi < lo) std::swap(lo, hi);
    const std::uint64_t span = static_cast<std::uint64_t>(hi - lo) + 1ULL;
    const std::uint64_t r = next_u64();
    return lo + static_cast<int>(r % span);
  }

  double range(double lo_incl, double hi_incl) {
    double lo = lo_incl;
    double hi = hi_incl;
    if (hi < lo) std::swap(lo, hi);
    return lo + (hi - lo) * next_u01();
  }

  // Back-compat alias: some call sites use range_real().
  double range_real(double lo_incl, double hi_incl) { return range(lo_incl, hi_incl); }
};

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
  s ^= fnv1a_64(a.kind) * 0x94d049bb133111ebULL;
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

  const int fmt = rng.range_int(0, 3);

  auto make_tag = [&]() {
    const std::string code = anomaly_signature_code(a);
    // Keep tag compact.
    return std::string("[") + code + "]";
  };

  if (a.kind == "distress") {
    const char* head = pick_from(kDistress, rng);
    // Callsign-like suffix.
    const std::string call = hex_n(rng.next_u64() ^ s, 4);
    if (fmt == 0) return std::string(head) + " " + call;
    if (fmt == 1) return std::string(head) + " " + call + " " + make_tag();
    return std::string(head) + ": " + call;
  }

  const bool is_ruins = (a.kind == "ruins" || a.kind == "artifact");
  const bool is_phen = (a.kind == "phenomenon");
  const char* node = is_ruins ? pick_from(kRuins, rng) : (is_phen ? pick_from(kPhenom, rng) : pick_from(kSignal, rng));

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

  std::string line;
  line.reserve(160);

  // Kind-specific sentence.
  if (a.kind == "ruins" || a.kind == "artifact") {
    line = std::string(pick_from(kRuinsA, rng)) + " " + pick_from(kRuinsB, rng);
    if (ruins > 0.55) line += "; the site feels intentionally concealed";
  } else if (a.kind == "phenomenon") {
    line = std::string(pick_from(kPhenomA, rng)) + " " + pick_from(kPhenomB, rng);
    if (neb > 0.60) line += "; nebular ions amplify the effect";
  } else if (a.kind == "distress") {
    line = std::string(pick_from(kDistA, rng)) + " " + pick_from(kDistB, rng);
    if (pir > 0.55) line += "; analysts warn of pirate spoofing";
  } else {
    line = std::string(pick_from(kSignalA, rng)) + " " + pick_from(kSignalB, rng);
    if (neb > 0.50) line += "; signal is smeared by nebula haze";
  }

  // Theme tag (tiny) + signature.
  if (!theme.empty()) {
    line += ". Theme tag: " + theme;
  }
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
