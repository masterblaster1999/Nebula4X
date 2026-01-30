#pragma once

// Internal helpers shared across Simulation translation units.
//
// This header is intentionally *not* installed as part of the public API.
// It exists to keep simulation.cpp maintainable and to improve incremental
// build times by allowing large Simulation methods to live in separate .cpp
// files without duplicating utility code.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <utility>

#include "nebula4x/core/game_state.h"

namespace nebula4x::sim_internal {

inline constexpr double kTwoPi = 6.283185307179586476925286766559;

inline std::string ascii_to_lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return s;
}

template <typename T>
inline void push_unique(std::vector<T>& v, const T& x) {
  if (std::find(v.begin(), v.end(), x) == v.end()) v.push_back(x);
}

template <typename T>
inline bool vec_contains(const std::vector<T>& v, const T& x) {
  return std::find(v.begin(), v.end(), x) != v.end();
}

inline bool is_mining_installation(const InstallationDef& def) {
  if (def.mining) return true;
  // Back-compat heuristic: if the content didn't explicitly set the flag,
  // treat installations whose id contains "mine" and that produce minerals as miners.
  if (def.produces_per_day.empty()) return false;
  const std::string lid = ascii_to_lower(def.id);
  return lid.find("mine") != std::string::npos;
}

inline bool faction_has_tech(const Faction& f, const std::string& tech_id) {
  return std::find(f.known_techs.begin(), f.known_techs.end(), tech_id) != f.known_techs.end();
}

inline double mkm_per_day_from_speed(double speed_km_s, double seconds_per_day) {
  const double km_per_day = speed_km_s * seconds_per_day;
  return km_per_day / 1.0e6; // million km
}

// --- Geometric line-of-sight helpers ---
//
// Some systems (sensors, beam weapons) can optionally require a clear
// line-of-sight that is not blocked by the physical radii of celestial bodies.
//
// NOTE: We keep these helpers here (internal header) so multiple simulation
// translation units can share a single implementation without inflating the
// public API surface.

inline bool segment_intersects_circle(const Vec2& a, const Vec2& b, const Vec2& center, double radius_mkm) {
  if (!(radius_mkm > 0.0) || !std::isfinite(radius_mkm)) return false;

  const Vec2 ab = b - a;
  const double ab2 = ab.x * ab.x + ab.y * ab.y;

  // Degenerate segment: treat as a point test.
  if (ab2 <= 1e-18) {
    const Vec2 d = a - center;
    const double d2 = d.x * d.x + d.y * d.y;
    return d2 <= radius_mkm * radius_mkm + 1e-12;
  }

  // Project center onto segment.
  const Vec2 ac = center - a;
  double t = (ac.x * ab.x + ac.y * ab.y) / ab2;
  if (!std::isfinite(t)) t = 0.0;
  t = std::clamp(t, 0.0, 1.0);

  const Vec2 p = a + ab * t;
  const Vec2 d = p - center;
  const double d2 = d.x * d.x + d.y * d.y;
  return d2 <= radius_mkm * radius_mkm + 1e-12;
}

// Returns true if the segment from `from_mkm` to `to_mkm` is blocked by any
// non-stellar body in the given system.
//
// We intentionally ignore BodyType::Star because the simulation is 2D; treating
// the star as a hard occluder would create unrealistic artifacts where ships on
// opposite sides of a system are always occluded by the star.
inline bool system_line_of_sight_blocked_by_bodies(const GameState& s, Id system_id,
                                                  const Vec2& from_mkm, const Vec2& to_mkm,
                                                  double padding_mkm = 0.0) {
  auto it_sys = s.systems.find(system_id);
  if (it_sys == s.systems.end()) return false;

  const auto& sys = it_sys->second;
  if (sys.bodies.empty()) return false;

  const double pad = (std::isfinite(padding_mkm) && padding_mkm > 0.0) ? padding_mkm : 0.0;

  // Ignore bodies that contain the endpoints. Ships/colonies in orbit are
  // represented at the body's position for simplicity; without this guard,
  // a planet would always "occlude" LOS to/from anything orbiting it.
  constexpr double kEndpointEpsMkm = 1e-6;

  for (Id bid : sys.bodies) {
    auto it_b = s.bodies.find(bid);
    if (it_b == s.bodies.end()) continue;

    const Body& body = it_b->second;
    if (body.system_id != system_id) continue;
    if (body.type == BodyType::Star) continue;

    double r_km = body.radius_km;
    if (!std::isfinite(r_km) || r_km <= 0.0) continue;

    double r_mkm = r_km * 1.0e-6 + pad;
    if (!std::isfinite(r_mkm) || r_mkm <= 0.0) continue;

    const Vec2 c = body.position_mkm;
    const double r_eff = r_mkm + kEndpointEpsMkm;
    const double r_eff2 = r_eff * r_eff;

    const Vec2 da = from_mkm - c;
    const Vec2 db = to_mkm - c;
    const double da2 = da.x * da.x + da.y * da.y;
    const double db2 = db.x * db.x + db.y * db.y;
    if (da2 <= r_eff2 || db2 <= r_eff2) continue;

    if (segment_intersects_circle(from_mkm, to_mkm, c, r_mkm)) return true;
  }

  return false;
}

inline bool system_line_of_sight_clear_by_bodies(const GameState& s, Id system_id,
                                                const Vec2& from_mkm, const Vec2& to_mkm,
                                                double padding_mkm = 0.0) {
  return !system_line_of_sight_blocked_by_bodies(s, system_id, from_mkm, to_mkm, padding_mkm);
}

// Many core containers are stored as std::unordered_map for convenience.
// Iteration order of unordered_map is not specified, so relying on it can
// introduce cross-platform nondeterminism.
template <typename Map>
inline std::vector<typename Map::key_type> sorted_keys(const Map& m) {
  std::vector<typename Map::key_type> keys;
  keys.reserve(m.size());
  for (const auto& [k, _] : m) keys.push_back(k);
  std::sort(keys.begin(), keys.end());
  return keys;
}

// Deterministic reductions for unordered_map-like containers.
//
// Even when a reduction is mathematically commutative (sum), floating-point
// arithmetic is not associative, and unordered_map iteration order is not
// specified. Sorting keys first gives stable, cross-platform accumulation
// order and makes simulation outcomes easier to reproduce.
//
// Returns the sum of all finite, positive mapped values in deterministic key
// order. Uses long double for improved numeric stability.
template <typename Map>
inline long double stable_sum_nonneg_sorted_ld(const Map& m) {
  long double acc = 0.0L;
  const auto keys = sorted_keys(m);
  for (const auto& k : keys) {
    auto it = m.find(k);
    if (it == m.end()) continue;
    const long double v = static_cast<long double>(it->second);
    const double dv = static_cast<double>(v);
    if (std::isnan(dv) || std::isinf(dv)) continue;
    if (dv > 0.0) acc += v;
  }
  return acc;
}

template <typename Map>
inline double stable_sum_nonneg_sorted(const Map& m) {
  return static_cast<double>(stable_sum_nonneg_sorted_ld(m));
}

// Treaty helpers.
//
// Treaties are stored in GameState with faction ids normalized (faction_a < faction_b).
// These helpers are intentionally small/cheap and are used to gate some
// gameplay actions (e.g. issuing hostile orders) on active diplomatic treaties.
inline int treaty_strength(TreatyType t) {
  switch (t) {
    case TreatyType::Alliance: return 5;
    case TreatyType::TradeAgreement: return 4;
    case TreatyType::ResearchAgreement: return 3;
    case TreatyType::NonAggressionPact: return 2;
    case TreatyType::Ceasefire: return 1;
  }
  return 0;
}

inline bool treaty_is_active(const Treaty& t, std::int64_t now_day) {
  const int dur = t.duration_days;
  if (dur > 0) {
    const std::int64_t end_day = t.start_day + static_cast<std::int64_t>(dur);
    return now_day < end_day;
  }
  // duration_days <= 0 => indefinite (or legacy/invalid values treated as indefinite).
  return true;
}

// Returns true if there is any active treaty between the two factions.
// If out_strongest_type is non-null, it is set to the "strongest" active treaty type.
// Strength order (high -> low): Alliance, TradeAgreement, ResearchAgreement, NonAggressionPact, Ceasefire.
inline bool strongest_active_treaty_between(const GameState& s, Id faction_a, Id faction_b,
                                           TreatyType* out_strongest_type = nullptr) {
  if (faction_a == kInvalidId || faction_b == kInvalidId) return false;
  if (faction_a == faction_b) return false;
  if (s.treaties.empty()) return false;

  Id a = faction_a;
  Id b = faction_b;
  if (b < a) std::swap(a, b);

  const std::int64_t now = s.date.days_since_epoch();
  int best = -1;
  TreatyType best_type = TreatyType::Ceasefire;
  bool found = false;

  for (const auto& [_, t] : s.treaties) {
    if (t.faction_a != a || t.faction_b != b) continue;
    if (!treaty_is_active(t, now)) continue;
    const int str = treaty_strength(t.type);
    if (!found || str > best) {
      found = true;
      best = str;
      best_type = t.type;
    }
  }

  if (found && out_strongest_type) *out_strongest_type = best_type;
  return found;
}

// Power allocation helpers.
//
// The core algorithm lives in nebula4x/core/power.h (public, used by both UI
// and simulation). We keep small wrappers here so simulation translation units
// can share the same helpers without duplicating implementation.
using nebula4x::PowerAllocation;
using nebula4x::ShipPowerPolicy;

inline PowerAllocation compute_power_allocation(const ShipDesign& d, const ShipPowerPolicy& policy) {
  return nebula4x::compute_power_allocation(d.power_generation, d.power_use_engines, d.power_use_shields,
                                            d.power_use_weapons, d.power_use_sensors, policy);
}

inline PowerAllocation compute_power_allocation(const ShipDesign& d) {
  return compute_power_allocation(d, ShipPowerPolicy{});
}

// --- Faction economy modifiers ---
//
// Tech effects can apply simple, faction-wide multipliers to economic outputs.
// This is intentionally lightweight so content authors can prototype
// "+10% mining" or "+15% research" style techs without needing new component
// defs.
//
// Supported tech effect encodings (case-insensitive):
//   {"type":"faction_output_bonus", "value":"mining", "amount":0.10}
//     -> multiplies mining output by (1 + amount)
//   {"type":"faction_output_multiplier", "value":"research", "amount":1.15}
//     -> multiplies research output by amount
//
// value can be one of:
//   "all", "mining", "industry", "research", "construction", "shipyard",
//   "terraforming", "troop_training".
struct FactionEconomyMultipliers {
  double mining{1.0};
  double industry{1.0};
  double research{1.0};
  double construction{1.0};
  double shipyard{1.0};
  double terraforming{1.0};
  double troop_training{1.0};
};

inline double clamp_factor(double f) {
  if (!std::isfinite(f)) return 1.0;
  if (f < 0.0) return 0.0;
  return f;
}

inline void apply_factor(FactionEconomyMultipliers& m, const std::string& key, double factor) {
  if (key.empty() || key == "all") {
    m.mining *= factor;
    m.industry *= factor;
    m.research *= factor;
    m.construction *= factor;
    m.shipyard *= factor;
    m.terraforming *= factor;
    m.troop_training *= factor;
    return;
  }
  if (key == "mining") {
    m.mining *= factor;
    return;
  }
  if (key == "industry") {
    m.industry *= factor;
    return;
  }
  if (key == "research") {
    m.research *= factor;
    return;
  }
  if (key == "construction" || key == "construction_points" || key == "construction_point") {
    m.construction *= factor;
    return;
  }
  if (key == "shipyard") {
    m.shipyard *= factor;
    return;
  }
  if (key == "terraforming") {
    m.terraforming *= factor;
    return;
  }
  if (key == "troop_training" || key == "training") {
    m.troop_training *= factor;
    return;
  }
}

inline FactionEconomyMultipliers compute_faction_economy_multipliers(const ContentDB& content, const Faction& f) {
  FactionEconomyMultipliers out;
  for (const auto& tech_id : f.known_techs) {
    auto it = content.techs.find(tech_id);
    if (it == content.techs.end()) continue;
    const auto& tech = it->second;

    for (const auto& eff : tech.effects) {
      const std::string type = ascii_to_lower(eff.type);
      const std::string key = ascii_to_lower(eff.value);

      double factor = 1.0;
      if (type == "faction_output_bonus" || type == "faction_economy_bonus") {
        // bonus is expressed as a fraction (+0.10 == +10%).
        factor = 1.0 + eff.amount;
      } else if (type == "faction_output_multiplier" || type == "faction_economy_multiplier") {
        factor = eff.amount;
      } else {
        continue;
      }

      factor = clamp_factor(factor);
      if (factor <= 0.0) continue;
      apply_factor(out, key, factor);
    }
  }
  // Clamp any NaN/inf that slipped through.
  out.mining = clamp_factor(out.mining);
  out.industry = clamp_factor(out.industry);
  out.research = clamp_factor(out.research);
  out.construction = clamp_factor(out.construction);
  out.shipyard = clamp_factor(out.shipyard);
  out.terraforming = clamp_factor(out.terraforming);
  out.troop_training = clamp_factor(out.troop_training);
  return out;
}

// --- Treaty / diplomacy derived modifiers ---
//
// Treaties can unlock economic multipliers (e.g. trade) and intel sharing
// between factions (e.g. alliances). These helpers live here so multiple
// Simulation translation units can share deterministic implementations.

inline constexpr double kTradeAgreementBonusPerPartner = 0.05;  // +5% per partner
inline constexpr double kTradeAgreementBonusCap = 0.25;         // cap at +25%

inline int count_trade_partners(const GameState& s, Id faction_id, bool include_alliances = true) {
  if (faction_id == kInvalidId) return 0;
  if (s.treaties.empty()) return 0;

  std::vector<Id> partners;
  partners.reserve(s.treaties.size());

  for (const auto& [_, t] : s.treaties) {
    (void)_;  // deterministic: order doesn't matter; we sort/unique via push_unique.
    const bool is_trade = (t.type == TreatyType::TradeAgreement);
    const bool is_alliance = (t.type == TreatyType::Alliance);
    if (!is_trade && !(include_alliances && is_alliance)) continue;

    if (t.faction_a == faction_id) push_unique(partners, t.faction_b);
    if (t.faction_b == faction_id) push_unique(partners, t.faction_a);
  }

  return static_cast<int>(partners.size());
}

inline double trade_agreement_output_multiplier(const GameState& s, Id faction_id) {
  const int n = count_trade_partners(s, faction_id, /*include_alliances=*/true);
  if (n <= 0) return 1.0;
  const double bonus = std::min(kTradeAgreementBonusCap, kTradeAgreementBonusPerPartner * static_cast<double>(n));
  return 1.0 + std::max(0.0, bonus);
}

struct IntelSyncDelta {
  int added_a_systems = 0;
  int added_b_systems = 0;
  int added_a_jumps = 0;
  int added_b_jumps = 0;
  int merged_a_contacts = 0;
  int merged_b_contacts = 0;
  bool route_cache_dirty = false;
};

// Deterministically merge map knowledge (systems + surveyed jump points) and,
// optionally, ship contact intel between two factions.
//
// NOTE: This does *not* invalidate any Simulation caches (callers must).
inline IntelSyncDelta sync_intel_between_factions(GameState& s, Id faction_a, Id faction_b, bool share_contacts) {
  IntelSyncDelta d;
  if (faction_a == kInvalidId || faction_b == kInvalidId) return d;
  if (faction_a == faction_b) return d;

  auto* fa = find_ptr(s.factions, faction_a);
  auto* fb = find_ptr(s.factions, faction_b);
  if (!fa || !fb) return d;

  auto merge_systems = [&](Faction& dst, const Faction& src, int& added) {
    for (Id sid : src.discovered_systems) {
      if (sid == kInvalidId) continue;
      if (std::find(dst.discovered_systems.begin(), dst.discovered_systems.end(), sid) != dst.discovered_systems.end()) {
        continue;
      }
      dst.discovered_systems.push_back(sid);
      added += 1;
    }
  };

  auto merge_jump_surveys = [&](Faction& dst, const Faction& src, int& added) {
    for (Id jid : src.surveyed_jump_points) {
      if (jid == kInvalidId) continue;
      if (std::find(dst.surveyed_jump_points.begin(), dst.surveyed_jump_points.end(), jid) !=
          dst.surveyed_jump_points.end()) {
        continue;
      }
      dst.surveyed_jump_points.push_back(jid);
      added += 1;
    }
  };

  auto merge_contacts = [&](Faction& dst, const Faction& src, int& merged) {
    const auto keys = sorted_keys(src.ship_contacts);
    for (Id sid : keys) {
      const auto it_src = src.ship_contacts.find(sid);
      if (it_src == src.ship_contacts.end()) continue;
      const Contact& c = it_src->second;
      // Don't import contacts for our own ships.
      if (c.last_seen_faction_id == dst.id) continue;

      auto it_dst = dst.ship_contacts.find(sid);
      if (it_dst == dst.ship_contacts.end()) {
        dst.ship_contacts[sid] = c;
        merged += 1;
      } else if (c.last_seen_day > it_dst->second.last_seen_day) {
        it_dst->second = c;
        merged += 1;
      }
    }
  };

  // Build the union (A<-B then B<-A) to preserve existing counting semantics.
  merge_systems(*fa, *fb, d.added_a_systems);
  merge_systems(*fb, *fa, d.added_b_systems);

  merge_jump_surveys(*fa, *fb, d.added_a_jumps);
  merge_jump_surveys(*fb, *fa, d.added_b_jumps);

  if (share_contacts) {
    merge_contacts(*fa, *fb, d.merged_a_contacts);
    merge_contacts(*fb, *fa, d.merged_b_contacts);
  }

  d.route_cache_dirty = (d.added_a_systems + d.added_b_systems + d.added_a_jumps + d.added_b_jumps) > 0;
  return d;
}

inline std::uint64_t double_bits(double v) {
  std::uint64_t out = 0;
  static_assert(sizeof(out) == sizeof(v));
  std::memcpy(&out, &v, sizeof(out));
  return out;
}

} // namespace nebula4x::sim_internal
