#include "nebula4x/core/simulation.h"

#include "simulation_internal.h"

#include "simulation_sensors.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstring>
#include <iomanip>
#include <limits>
#include <map>
#include <optional>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

#include "nebula4x/core/scenario.h"
#include "nebula4x/core/ai_economy.h"
#include "nebula4x/util/log.h"
#include "nebula4x/util/trace_events.h"
#include "nebula4x/util/spatial_index.h"

namespace nebula4x {
namespace {
using sim_internal::kTwoPi;
using sim_internal::ascii_to_lower;
using sim_internal::is_mining_installation;
using sim_internal::mkm_per_day_from_speed;
using sim_internal::push_unique;
using sim_internal::vec_contains;
using sim_internal::sorted_keys;
using sim_internal::faction_has_tech;
using sim_internal::FactionEconomyMultipliers;
using sim_internal::compute_faction_economy_multipliers;
using sim_internal::sync_intel_between_factions;
using sim_internal::compute_power_allocation;

using sim_sensors::SensorSource;
using sim_sensors::gather_sensor_sources;


// Deterministic tiny RNG helpers (used for daily environmental events).
inline std::uint32_t hash_u32(std::uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352dU;
  x ^= x >> 15;
  x *= 0x846ca68bU;
  x ^= x >> 16;
  return x;
}

inline double hash_to_unit01(std::uint32_t x) {
  // Map to [0,1). Use 24 bits to avoid denorms.
  return static_cast<double>(x & 0x00FFFFFFu) / static_cast<double>(0x01000000u);
}

} // namespace

void Simulation::recompute_body_positions() {
  NEBULA4X_TRACE_SCOPE("recompute_body_positions", "sim");
  const double t = static_cast<double>(state_.date.days_since_epoch()) +
                   static_cast<double>(std::clamp(state_.hour_of_day, 0, 23)) / 24.0;

  // Bodies may orbit other bodies (e.g., moons). Compute absolute positions in a
  // parent-first manner, but remain robust to unordered_map iteration order.
  std::unordered_map<Id, Vec2> cache;
  cache.reserve(state_.bodies.size() * 2);

  std::unordered_set<Id> visiting;
  visiting.reserve(state_.bodies.size());

  const auto compute_pos = [&](Id id, const auto& self) -> Vec2 {
    if (id == kInvalidId) return {0.0, 0.0};

    if (auto it = cache.find(id); it != cache.end()) return it->second;

    auto itb = state_.bodies.find(id);
    if (itb == state_.bodies.end()) return {0.0, 0.0};

    Body& b = itb->second;

    // Break accidental cycles gracefully (treat as orbiting system origin).
    if (!visiting.insert(id).second) {
      cache[id] = {0.0, 0.0};
      return {0.0, 0.0};
    }

    // Orbit center: either system origin or a parent body's current position.
    Vec2 center{0.0, 0.0};
    if (b.parent_body_id != kInvalidId && b.parent_body_id != id) {
      const Body* parent = find_ptr(state_.bodies, b.parent_body_id);
      if (parent && parent->system_id == b.system_id) {
        center = self(b.parent_body_id, self);
      }
    }

    Vec2 pos = center;
    if (b.orbit_radius_mkm > 1e-9) {
      const double a = std::max(0.0, b.orbit_radius_mkm);
      const double e = std::clamp(b.orbit_eccentricity, 0.0, 0.999999);
      const double period = std::max(1.0, b.orbit_period_days);

      // Mean anomaly advances linearly with time.
      double M = b.orbit_phase_radians + kTwoPi * (t / period);
      // Wrap for numerical stability.
      M = std::fmod(M, kTwoPi);
      if (M < 0.0) M += kTwoPi;

      // Solve Kepler's equation: M = E - e sin(E) for eccentric anomaly E.
      // Newton iteration converges quickly for typical orbital eccentricities.
      double E = (e < 0.8) ? M : (kTwoPi * 0.5); // start at pi for high-e orbits
      for (int it = 0; it < 12; ++it) {
        const double sE = std::sin(E);
        const double cE = std::cos(E);
        const double f = (E - e * sE) - M;
        const double fp = 1.0 - e * cE;
        if (std::fabs(fp) < 1e-12) break;
        const double d = f / fp;
        E -= d;
        if (std::fabs(f) < 1e-10) break;
      }

      const double sE = std::sin(E);
      const double cE = std::cos(E);
      const double bsemi = a * std::sqrt(std::max(0.0, 1.0 - e * e));
      const double x = a * (cE - e);
      const double y = bsemi * sE;

      const double w = b.orbit_arg_periapsis_radians;
      const double cw = std::cos(w);
      const double sw = std::sin(w);
      const double rx = x * cw - y * sw;
      const double ry = x * sw + y * cw;

      pos = center + Vec2{rx, ry};
    }

    cache[id] = pos;
    visiting.erase(id);
    return pos;
  };

  for (auto& [id, b] : state_.bodies) {
    b.position_mkm = compute_pos(id, compute_pos);
  }
}

void Simulation::tick_one_day() {
  NEBULA4X_TRACE_SCOPE("tick_one_day", "sim");
  // A "day" is 24 hours, even if the simulation is currently mid-day.
  advance_hours(24);
}

void Simulation::tick_one_tick_hours(int hours) {
  NEBULA4X_TRACE_SCOPE("tick_one_tick_hours", "sim");
  if (hours <= 0) return;
  hours = std::clamp(hours, 1, 24);

  // If the game has ended, freeze simulation time/processing.
  // (The UI may still inspect the final state.)
  if (state_.victory_state.game_over) return;

  // Elimination victory can otherwise be "dodged" within a single day by
  // colonizing again before the midnight evaluation. To keep elimination
  // semantics intuitive, we evaluate elimination-only rules immediately at the
  // start of each tick.
  if (state_.victory_rules.enabled && state_.victory_rules.elimination_enabled &&
      state_.victory_rules.score_threshold <= 0.0) {
    tick_victory();
    if (state_.victory_state.game_over) return;
  }

  // Defensive: if a caller asks for a tick that crosses more than one day
  // boundary, split it.
  const int start_hod = std::clamp(state_.hour_of_day, 0, 23);
  if (start_hod + hours > 24) {
    const int first = 24 - start_hod;
    tick_one_tick_hours(first);
    tick_one_tick_hours(hours - first);
    return;
  }

  const int prev_day = static_cast<int>(state_.date.days_since_epoch());
  const int end_hod_raw = start_hod + hours;

  // Advance time.
  if (end_hod_raw == 24) {
    state_.hour_of_day = 0;
    state_.date = state_.date.add_days(1);
  } else {
    state_.hour_of_day = end_hod_raw;
  }

  const bool day_advanced = static_cast<int>(state_.date.days_since_epoch()) != prev_day;

  // Update moving bodies at the new simulation time.
  recompute_body_positions();

  const double dt_days = static_cast<double>(hours) / 24.0;

  // Daily environmental updates (midnight boundary).
  if (day_advanced) tick_nebula_storms();

  if (cfg_.enable_subday_economy) {
    // Economy ticks every step (scaled by dt_days).
    //
    // Some warnings (e.g. habitation shortfall) are intentionally throttled to
    // daily cadence via emit_daily_events to avoid spamming the event log when
    // running at 1h/6h/12h resolution.
    tick_colonies(dt_days, day_advanced);
    tick_research(dt_days);
    tick_shipyards(dt_days);
    tick_construction(dt_days);

    // Keep AI as a daily tick for now (avoid thrashing decisions every hour).
    if (day_advanced) {
      tick_treaties();
      tick_diplomatic_offers();
      tick_ai();
    }
  } else if (day_advanced) {
    // Daily economy / planning ticks (midnight boundary).
    tick_colonies(1.0, /*emit_daily_events=*/true);
    tick_research(1.0);
    tick_shipyards(1.0);
    tick_construction(1.0);
    tick_treaties();
    tick_diplomatic_offers();
    tick_ai();
    tick_refuel();
    tick_rearm();
    tick_ship_maintenance(1.0);
  }

  // Continuous (sub-day) ticks.
  tick_heat(dt_days);
  tick_ships(dt_days);
  tick_contacts(dt_days, day_advanced);
  tick_shields(dt_days);
  if (cfg_.enable_combat) tick_combat(dt_days);

  // Post-movement maintenance / slow processes.
  if (cfg_.enable_subday_economy) {
    tick_refuel();
    tick_rearm();
    tick_ship_maintenance(dt_days);
    tick_crew_training(dt_days);
    tick_terraforming(dt_days);
    tick_repairs(dt_days);
    if (day_advanced) {
      tick_ground_combat();
      tick_ship_maintenance_failures();
    }
  } else if (day_advanced) {
    tick_ground_combat();
    tick_terraforming(1.0);
    tick_repairs(1.0);
    tick_crew_training(1.0);
    tick_ship_maintenance_failures();

    // Wreck cleanup (optional).
    if (cfg_.wreck_decay_days > 0 && !state_.wrecks.empty()) {
      const std::int64_t now = state_.date.days_since_epoch();
      const std::int64_t max_age = cfg_.wreck_decay_days;
      for (auto it = state_.wrecks.begin(); it != state_.wrecks.end();) {
        const std::int64_t created = it->second.created_day;
        const std::int64_t age = (created > 0) ? (now - created) : 0;
        if (age >= max_age) {
          it = state_.wrecks.erase(it);
        } else {
          ++it;
        }
      }
    }
  }

  // In sub-day economy mode we still only prune wrecks on day boundaries (keeps
  // behavior stable and avoids doing extra work every hour).
  if (cfg_.enable_subday_economy && day_advanced) {
    if (cfg_.wreck_decay_days > 0 && !state_.wrecks.empty()) {
      const std::int64_t now = state_.date.days_since_epoch();
      const std::int64_t max_age = cfg_.wreck_decay_days;
      for (auto it = state_.wrecks.begin(); it != state_.wrecks.end();) {
        const std::int64_t created = it->second.created_day;
        const std::int64_t age = (created > 0) ? (now - created) : 0;
        if (age >= max_age) {
          it = state_.wrecks.erase(it);
        } else {
          ++it;
        }
      }
    }
  }

  // Victory conditions are evaluated on day boundaries so that all daily
  // effects (combat, invasion results, economy) have already been applied.
  if (day_advanced) {
    // Dynamic procedural points-of-interest are spawned before contracts so that
    // newly-created anomalies/caches can be picked up by the mission board once
    // they are discovered.
    tick_dynamic_points_of_interest();

    // Procedural contracts (mission board) are evaluated at day boundaries so
    // completion/expiration happens after all simulation effects.
    tick_contracts();
    tick_victory();
  }
}

void Simulation::tick_victory() {
  // No-op if disabled or already ended.
  if (!state_.victory_rules.enabled) return;
  if (state_.victory_state.game_over) return;

  // If no victory mode is enabled, there is nothing to do.
  if (!state_.victory_rules.elimination_enabled && state_.victory_rules.score_threshold <= 0.0) return;

  // Fast path: elimination-only rules (score victory disabled).
  // This is called both on the daily boundary and opportunistically at the
  // start of ticks to prevent "revive before evaluation" edge cases.
  if (state_.victory_rules.elimination_enabled && state_.victory_rules.score_threshold <= 0.0) {
    const VictoryRules& rules = state_.victory_rules;

    // Track which factions currently own any colony (and optionally any ship).
    std::unordered_set<Id> has_colony;
    has_colony.reserve(state_.factions.size() * 2 + 8);
    for (const auto& [cid, c] : state_.colonies) {
      (void)cid;
      if (c.faction_id == kInvalidId) continue;
      has_colony.insert(c.faction_id);
    }

    std::unordered_set<Id> has_ship;
    if (!rules.elimination_requires_colony) {
      has_ship.reserve(state_.factions.size() * 2 + 8);
      for (const auto& [sid, sh] : state_.ships) {
        (void)sid;
        if (sh.faction_id == kInvalidId) continue;
        has_ship.insert(sh.faction_id);
      }
    }

    int eligible_total = 0;
    int eligible_alive = 0;
    Id last_alive_id = kInvalidId;

    for (Id fid : sorted_keys(state_.factions)) {
      const auto& f = state_.factions.at(fid);
      // Passive factions are neutral ambient entities and are not intended to
      // participate in victory conditions.
      const bool eligible =
          !(rules.exclude_pirates && f.control == FactionControl::AI_Pirate) &&
          (f.control != FactionControl::AI_Passive);
      if (!eligible) continue;

      eligible_total++;

      const bool alive = rules.elimination_requires_colony
                             ? has_colony.contains(fid)
                             : (has_colony.contains(fid) || has_ship.contains(fid));
      if (alive) {
        eligible_alive++;
        last_alive_id = fid;
      }
    }

    // Don't auto-win in single-faction sandboxes.
    if (eligible_total < 2) return;

    if (eligible_alive == 1 && last_alive_id != kInvalidId) {
      const auto* winner = find_ptr(state_.factions, last_alive_id);
      state_.victory_state.game_over = true;
      state_.victory_state.winner_faction_id = last_alive_id;
      state_.victory_state.reason = VictoryReason::LastFactionStanding;
      state_.victory_state.victory_day = state_.date.days_since_epoch();
      state_.victory_state.winner_score = 0.0;

      std::string msg = "Victory: ";
      msg += (winner ? winner->name : std::to_string(static_cast<unsigned long long>(last_alive_id)));
      msg += " wins by elimination.";
      this->push_event(EventLevel::Warn, EventCategory::General, msg,
                       EventContext{.faction_id = last_alive_id,
                                    .faction_id2 = kInvalidId,
                                    .system_id = kInvalidId,
                                    .ship_id = kInvalidId,
                                    .colony_id = kInvalidId});
    }
    return;
  }

  const auto scores = compute_scoreboard(state_.victory_rules);
  if (scores.empty()) return;

  // Determine eligible competitors.
  int eligible_total = 0;
  int eligible_alive = 0;
  Id last_alive_id = kInvalidId;
  for (const auto& e : scores) {
    if (!e.eligible_for_victory) continue;
    eligible_total++;
    if (e.alive) {
      eligible_alive++;
      last_alive_id = e.faction_id;
    }
  }

  // Don't auto-win in single-faction sandboxes.
  if (eligible_total < 2) return;

  // --- Elimination victory ---
  if (state_.victory_rules.elimination_enabled && eligible_alive == 1 && last_alive_id != kInvalidId) {
    const auto* winner = find_ptr(state_.factions, last_alive_id);
    state_.victory_state.game_over = true;
    state_.victory_state.winner_faction_id = last_alive_id;
    state_.victory_state.reason = VictoryReason::LastFactionStanding;
    state_.victory_state.victory_day = state_.date.days_since_epoch();
    state_.victory_state.winner_score = 0.0;

    std::string msg = "Victory: ";
    msg += (winner ? winner->name : std::to_string(static_cast<unsigned long long>(last_alive_id)));
    msg += " wins by elimination.";
    this->push_event(EventLevel::Warn, EventCategory::General, msg,
                     EventContext{.faction_id = last_alive_id,
                                  .faction_id2 = kInvalidId,
                                  .system_id = kInvalidId,
                                  .ship_id = kInvalidId,
                                  .colony_id = kInvalidId});
    return;
  }

  // --- Score victory ---
  const double threshold = state_.victory_rules.score_threshold;
  if (threshold > 0.0) {
    // Find top two eligible factions by score.
    const ScoreboardEntry* best = nullptr;
    const ScoreboardEntry* second = nullptr;
    for (const auto& e : scores) {
      if (!e.eligible_for_victory) continue;
      if (!best) {
        best = &e;
        continue;
      }
      if (!second) {
        second = &e;
        continue;
      }
      break;
    }

    if (best) {
      const double best_score = best->score.total_points();
      const double second_score = second ? second->score.total_points() : 0.0;
      const double margin = state_.victory_rules.score_lead_margin;
      if (best_score >= threshold && best_score >= second_score + margin) {
        const auto* winner = find_ptr(state_.factions, best->faction_id);
        state_.victory_state.game_over = true;
        state_.victory_state.winner_faction_id = best->faction_id;
        state_.victory_state.reason = VictoryReason::ScoreThreshold;
        state_.victory_state.victory_day = state_.date.days_since_epoch();
        state_.victory_state.winner_score = best_score;

        std::string msg = "Victory: ";
        msg += (winner ? winner->name : std::to_string(static_cast<unsigned long long>(best->faction_id)));
        msg += " reaches the score threshold (";
        msg += std::to_string(static_cast<long long>(std::llround(best_score)));
        msg += " / ";
        msg += std::to_string(static_cast<long long>(std::llround(threshold)));
        msg += ").";
        this->push_event(EventLevel::Warn, EventCategory::General, msg,
                         EventContext{.faction_id = best->faction_id,
                                      .faction_id2 = kInvalidId,
                                      .system_id = kInvalidId,
                                      .ship_id = kInvalidId,
                                      .colony_id = kInvalidId});
      }
    }
  }
}



void Simulation::tick_nebula_storms() {
  if (!cfg_.enable_nebula_storms) return;

  const std::int64_t now = state_.date.days_since_epoch();

  // Track which systems have colonies for relevance filtering (storms that affect nothing are not announced).
  std::unordered_set<Id> systems_with_colonies;
  systems_with_colonies.reserve(state_.colonies.size() * 2 + 1);
  for (const auto& [cid, c] : state_.colonies) {
    (void)cid;
    const Body* b = find_ptr(state_.bodies, c.body_id);
    if (!b) continue;
    if (b->system_id != kInvalidId) systems_with_colonies.insert(b->system_id);
  }

  const auto sys_ids = sorted_keys(state_.systems);
  for (Id sid : sys_ids) {
    auto* sys = find_ptr(state_.systems, sid);
    if (!sys) continue;

    // Expire finished storms.
    if (sys->storm_peak_intensity > 0.0 && sys->storm_end_day > sys->storm_start_day && now >= sys->storm_end_day) {
      const bool important = !sys->ships.empty() || (systems_with_colonies.find(sid) != systems_with_colonies.end());
      if (important) {
        std::string msg = "Nebula storm dissipated in ";
        msg += sys->name;
        msg += ".";
        this->push_event(EventLevel::Info, EventCategory::Exploration, msg,
                         EventContext{.faction_id = kInvalidId,
                                      .faction_id2 = kInvalidId,
                                      .system_id = sid,
                                      .ship_id = kInvalidId,
                                      .colony_id = kInvalidId});
      }
      sys->storm_peak_intensity = 0.0;
      sys->storm_start_day = 0;
      sys->storm_end_day = 0;
    }

    // Skip if a storm is still active (or scheduled).
    if (sys->storm_peak_intensity > 0.0 && sys->storm_end_day > sys->storm_start_day && now < sys->storm_end_day) {
      continue;
    }

    // Consider starting a new storm.
    const double neb = std::clamp(sys->nebula_density, 0.0, 1.0);
    if (neb < cfg_.nebula_storm_min_nebula_density) continue;

    const double base = std::clamp(std::max(0.0, cfg_.nebula_storm_start_chance_per_day_at_max_density), 0.0, 1.0);
    const double exp = std::max(0.0, cfg_.nebula_storm_start_chance_exponent);

    double p = 0.0;
    if (base > 0.0) {
      // At low nebula density, storms should be much rarer.
      p = base * ((exp == 1.0) ? neb : std::pow(neb, exp));
      p = std::clamp(p, 0.0, 1.0);
    }
    if (p <= 0.0) continue;

    // Deterministic seed based on day + system id.
    const std::uint32_t seed = hash_u32(static_cast<std::uint32_t>(now) ^
                                        hash_u32(static_cast<std::uint32_t>(sid)) ^
                                        0x4E42554Cu /*'NBUL'*/);
    const double u = hash_to_unit01(seed);
    if (u >= p) continue;

    const double u_int = hash_to_unit01(hash_u32(seed ^ 0xA531u));
    const double u_dur = hash_to_unit01(hash_u32(seed ^ 0xBEEFu));

    int dur_min = std::max(1, cfg_.nebula_storm_duration_days_min);
    int dur_max = std::max(dur_min, cfg_.nebula_storm_duration_days_max);
    int dur = dur_min +
              static_cast<int>(std::floor(u_dur * static_cast<double>(dur_max - dur_min + 1)));
    dur = std::clamp(dur, dur_min, dur_max);

    double i_min = std::clamp(cfg_.nebula_storm_peak_intensity_min, 0.0, 1.0);
    double i_max = std::clamp(cfg_.nebula_storm_peak_intensity_max, 0.0, 1.0);
    if (i_max < i_min) std::swap(i_min, i_max);

    double peak = i_min + (i_max - i_min) * u_int;
    // Bias storm strength upward in very dense nebulae.
    peak = std::clamp(peak * (0.5 + 0.5 * neb), 0.0, 1.0);

    sys->storm_peak_intensity = peak;
    sys->storm_start_day = now;
    sys->storm_end_day = now + static_cast<std::int64_t>(dur);

    const bool important = !sys->ships.empty() || (systems_with_colonies.find(sid) != systems_with_colonies.end());
    if (important) {
      const int pct = static_cast<int>(std::llround(peak * 100.0));
      std::string msg = "Nebula storm forming in ";
      msg += sys->name;
      msg += " (peak ";
      msg += std::to_string(pct);
      msg += "%).";
      this->push_event(EventLevel::Info, EventCategory::Exploration, msg,
                       EventContext{.faction_id = kInvalidId,
                                    .faction_id2 = kInvalidId,
                                    .system_id = sid,
                                    .ship_id = kInvalidId,
                                    .colony_id = kInvalidId});
    }
  }
}


void Simulation::tick_treaties() {
  if (state_.treaties.empty()) return;

  const std::int64_t now = state_.date.days_since_epoch();

  auto type_title = [](TreatyType t) -> const char* {
    switch (t) {
      case TreatyType::Ceasefire: return "Ceasefire";
      case TreatyType::NonAggressionPact: return "Non-Aggression Pact";
      case TreatyType::Alliance: return "Alliance";
      case TreatyType::TradeAgreement: return "Trade Agreement";
    }
    return "Treaty";
  };

  for (auto it = state_.treaties.begin(); it != state_.treaties.end();) {
    const Treaty& t = it->second;
    const int dur = t.duration_days;
    if (dur > 0) {
      const std::int64_t end_day = t.start_day + static_cast<std::int64_t>(dur);
      if (now >= end_day) {
        const auto* fa = find_ptr(state_.factions, t.faction_a);
        const auto* fb = find_ptr(state_.factions, t.faction_b);

        std::string msg = "Treaty expired: ";
        msg += type_title(t.type);
        msg += " between ";
        msg += (fa ? fa->name : std::to_string(static_cast<unsigned long long>(t.faction_a)));
        msg += " and ";
        msg += (fb ? fb->name : std::to_string(static_cast<unsigned long long>(t.faction_b)));

        this->push_event(EventLevel::Info, EventCategory::Diplomacy, msg,
                         EventContext{.faction_id = t.faction_a,
                                      .faction_id2 = t.faction_b,
                                      .system_id = kInvalidId,
                                      .ship_id = kInvalidId,
                                      .colony_id = kInvalidId});

        it = state_.treaties.erase(it);
        continue;
      }
    }
    ++it;
  }

  // Ongoing intel sharing for treaties that imply chart exchange. We do this
  // once per day (here) rather than per sub-tick, and without events to avoid
  // spamming the log.
  //
  // Notes:
  // - Alliances share contacts in addition to maps.
  // - Trade Agreements exchange maps (discovered systems + surveyed jump points)
  //   but do not share contacts.
  // - Multiple treaties can exist between the same pair; pick the strongest
  //   sharing policy for the pair.
  std::map<std::pair<Id, Id>, bool> share_contacts_by_pair;
  for (Id tid : sorted_keys(state_.treaties)) {
    const Treaty& t = state_.treaties.at(tid);
    bool share_map = false;
    bool share_contacts = false;
    if (t.type == TreatyType::Alliance) {
      share_map = true;
      share_contacts = true;
    } else if (t.type == TreatyType::TradeAgreement) {
      share_map = true;
      share_contacts = false;
    }
    if (!share_map) continue;

    Id a = t.faction_a;
    Id b = t.faction_b;
    if (b < a) std::swap(a, b);
    const auto key = std::make_pair(a, b);
    auto it = share_contacts_by_pair.find(key);
    if (it == share_contacts_by_pair.end()) {
      share_contacts_by_pair.emplace(key, share_contacts);
    } else if (share_contacts) {
      // Upgrade (Trade -> Alliance).
      it->second = true;
    }
  }

  bool route_cache_dirty = false;
  for (const auto& [key, share_contacts] : share_contacts_by_pair) {
    const auto d = sync_intel_between_factions(state_, key.first, key.second, share_contacts);
    route_cache_dirty = route_cache_dirty || d.route_cache_dirty;
  }
  if (route_cache_dirty) {
    invalidate_jump_route_cache();
  }
}


void Simulation::tick_diplomatic_offers() {
  if (state_.diplomatic_offers.empty()) return;

  const int now_day = static_cast<int>(state_.date.days_since_epoch());
  std::vector<Id> expired;
  expired.reserve(state_.diplomatic_offers.size());

  for (const auto& [oid, o] : state_.diplomatic_offers) {
    (void)oid;
    if (o.expire_day >= 0 && now_day >= o.expire_day) expired.push_back(o.id);
  }

  if (expired.empty()) return;
  std::sort(expired.begin(), expired.end());

  for (Id oid : expired) {
    const DiplomaticOffer* o = find_ptr(state_.diplomatic_offers, oid);
    if (!o) continue;

    const auto* from = find_ptr(state_.factions, o->from_faction_id);
    const auto* to = find_ptr(state_.factions, o->to_faction_id);

    const bool player_involved = (from && from->control == FactionControl::Player) ||
                                 (to && to->control == FactionControl::Player);

    // Apply a small cooldown to avoid immediate re-offers after expiry.
    constexpr int kExpiredCooldownDays = 30;
    if (auto* f = find_ptr(state_.factions, o->from_faction_id)) {
      int& until = f->diplomacy_offer_cooldown_until_day[o->to_faction_id];
      until = std::max(until, now_day + kExpiredCooldownDays);
    }

    if (player_involved) {
      EventContext ctx;
      ctx.faction_id = o->to_faction_id;
      ctx.faction_id2 = o->from_faction_id;

      std::string msg = "Diplomatic offer expired";
      if (from) msg += " from " + from->name;
      this->push_event(EventLevel::Info, EventCategory::Diplomacy, std::move(msg), ctx);
    }

    state_.diplomatic_offers.erase(oid);
  }
}


void Simulation::push_event(EventLevel level, std::string message) {
  push_event(level, EventCategory::General, std::move(message), {});
}

void Simulation::push_event(EventLevel level, EventCategory category, std::string message, EventContext ctx) {
  SimEvent ev;
  ev.seq = state_.next_event_seq;
  state_.next_event_seq += 1;
  if (state_.next_event_seq == 0) state_.next_event_seq = 1; 

  ev.day = state_.date.days_since_epoch();
  ev.hour = std::clamp(state_.hour_of_day, 0, 23);
  ev.level = level;
  ev.category = category;
  ev.faction_id = ctx.faction_id;
  ev.faction_id2 = ctx.faction_id2;
  ev.system_id = ctx.system_id;
  ev.ship_id = ctx.ship_id;
  ev.colony_id = ctx.colony_id;
  ev.message = std::move(message);
  state_.events.push_back(std::move(ev));

  const int max_events = cfg_.max_events;
  if (max_events > 0 && static_cast<int>(state_.events.size()) > max_events + 128) {
    const std::size_t keep = static_cast<std::size_t>(max_events);
    const std::size_t cut = state_.events.size() - keep;
    state_.events.erase(state_.events.begin(), state_.events.begin() + static_cast<std::ptrdiff_t>(cut));
  }
}

void Simulation::push_journal_entry(Id faction_id, JournalEntry entry) {
  if (faction_id == kInvalidId) return;
  auto* fac = find_ptr(state_.factions, faction_id);
  if (!fac) return;
  if (entry.title.empty() && entry.text.empty()) return;

  entry.seq = (entry.seq == 0) ? state_.next_journal_seq : entry.seq;
  if (state_.next_journal_seq <= entry.seq) state_.next_journal_seq = entry.seq + 1;
  if (state_.next_journal_seq == 0) state_.next_journal_seq = 1;

  entry.day = state_.date.days_since_epoch();
  entry.hour = std::clamp(state_.hour_of_day, 0, 23);

  fac->journal.push_back(std::move(entry));

  // Journal is intended as a readable curated layer, so prune less aggressively.
  constexpr int kMaxJournalEntries = 2000;
  if ((int)fac->journal.size() > kMaxJournalEntries + 128) {
    const std::size_t keep = (std::size_t)kMaxJournalEntries;
    const std::size_t cut = fac->journal.size() - keep;
    fac->journal.erase(fac->journal.begin(), fac->journal.begin() + static_cast<std::ptrdiff_t>(cut));
  }
}

void Simulation::tick_contacts(double dt_days, bool emit_contact_lost_events) {
  NEBULA4X_TRACE_SCOPE("tick_contacts", "sim.sensors");
  dt_days = std::clamp(dt_days, 0.0, 1.0);
  const bool swept = (dt_days > 1e-9);
  const int now = static_cast<int>(state_.date.days_since_epoch());
  constexpr int kMaxContactAgeDays = 180;

  for (auto& [_, fac] : state_.factions) {
    for (auto it = fac.ship_contacts.begin(); it != fac.ship_contacts.end();) {
      const Contact& c = it->second;
      const bool dead = (state_.ships.find(c.ship_id) == state_.ships.end());
      const int age = now - c.last_seen_day;
      if (dead || age > kMaxContactAgeDays) {
        it = fac.ship_contacts.erase(it);
      } else {
        ++it;
      }
    }
  }

  struct Key {
    Id faction_id{kInvalidId};
    Id system_id{kInvalidId};
    bool operator==(const Key& o) const { return faction_id == o.faction_id && system_id == o.system_id; }
  };
  struct KeyHash {
    size_t operator()(const Key& k) const noexcept {
      // Avoid UB from shifting signed integers; Id is uint64_t.
      std::uint64_t h = 1469598103934665603ull;
      auto mix = [&](std::uint64_t v) {
        h ^= v;
        h *= 1099511628211ull;
      };
      mix(static_cast<std::uint64_t>(k.faction_id));
      mix(static_cast<std::uint64_t>(k.system_id));
      return static_cast<size_t>(h);
    }
  };

  std::unordered_map<Key, std::vector<SensorSource>, KeyHash> cache;

  auto sources_for = [&](Id faction_id, Id system_id) -> const std::vector<SensorSource>& {
    const Key key{faction_id, system_id};
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    auto sources = gather_sensor_sources(*this, faction_id, system_id);
    auto [ins, _ok] = cache.emplace(key, std::move(sources));
    return ins->second;
  };

  std::unordered_map<Id, std::vector<Id>> detected_today_by_faction;
  detected_today_by_faction.reserve(state_.factions.size());

  const auto faction_ids = sorted_keys(state_.factions);
  const auto system_ids = sorted_keys(state_.systems);

  // Build a compact list of (ship, viewer faction) detections for today.
  //
  // With sub-day turn ticks, ships can move through sensor range between the
  // start/end of the step. To avoid missing transient pass-bys (e.g. a fast
  // ship crossing a sensor bubble within a 24h step), we perform a swept test
  // using per-ship velocity vectors computed during tick_ships().
  //
  // We later sort by (ship_id, faction_id, t desc) to preserve deterministic
  // ordering while also retaining the most recent in-step snapshot when
  // multiple sources detect the same ship.
  struct DetectionRecord {
    Id ship_id{kInvalidId};
    Id viewer_faction_id{kInvalidId};
    // Fraction of the current tick in [0,1] at which the ship was "seen".
    // 1.0 corresponds to the end-of-tick position.
    double t{1.0};

    // Estimated 1-sigma position uncertainty (radius, mkm) at the seen time.
    //
    // This is used to seed Contact::last_seen_position_uncertainty_mkm.
    double uncertainty_mkm{0.0};
  };

  std::vector<DetectionRecord> detections;
  detections.reserve(std::min<std::size_t>(state_.ships.size() * 2, 4096));

  auto min_dist_sq_and_t = [](const Vec2& src0, const Vec2& src1, const Vec2& tgt0, const Vec2& tgt1,
                              double* out_t) -> double {
    const Vec2 ds = src1 - src0;
    const Vec2 dt = tgt1 - tgt0;
    const Vec2 d0 = tgt0 - src0;
    const Vec2 dv = dt - ds;
    const double dv2 = dv.x * dv.x + dv.y * dv.y;

    double t = 0.0;
    if (dv2 > 1e-18) {
      t = -(d0.x * dv.x + d0.y * dv.y) / dv2;
      t = std::clamp(t, 0.0, 1.0);
    }
    if (out_t) *out_t = t;

    const Vec2 d = d0 + dv * t;
    return d.x * d.x + d.y * d.y;
  };

  std::unordered_map<Id, SpatialIndex2D> system_index;
  system_index.reserve(state_.systems.size());

  auto index_for_system = [&](Id sys_id) -> SpatialIndex2D& {
    auto it = system_index.find(sys_id);
    if (it != system_index.end()) return it->second;
    SpatialIndex2D idx;
    if (const auto* sys = find_ptr(state_.systems, sys_id)) {
      idx.build_from_ship_ids(sys->ships, state_.ships);
    }
    auto [ins, _ok] = system_index.emplace(sys_id, std::move(idx));
    return ins->second;
  };

  const double max_sig = sim_sensors::max_signature_multiplier_for_detection(*this);

  const bool enable_uncertainty = cfg_.enable_contact_uncertainty;
  const double unc_frac_center = std::clamp(cfg_.contact_uncertainty_center_fraction_of_detect_range, 0.0, 1.0);
  const double unc_frac_edge = std::clamp(cfg_.contact_uncertainty_edge_fraction_of_detect_range, 0.0, 1.0);
  const double unc_frac_lo = std::min(unc_frac_center, unc_frac_edge);
  const double unc_frac_hi = std::max(unc_frac_center, unc_frac_edge);
  const double unc_min_mkm = std::max(0.0, cfg_.contact_uncertainty_min_mkm);
  const double unc_ecm_mult = std::max(0.0, cfg_.contact_uncertainty_ecm_strength_multiplier);
  const double unc_cap_mkm = cfg_.contact_uncertainty_max_mkm;

  for (Id sys_id : system_ids) {
    const auto* sys = find_ptr(state_.systems, sys_id);
    if (!sys) continue;
    if (sys->ships.empty()) continue;

    auto& idx = index_for_system(sys_id);

    // Conservative padding for the spatial query under swept detection.
    // If a target comes within range at any time during the interval, its end
    // position cannot be more than (range + |v_rel|*dt) away from the source's
    // end position. We bound |v_rel| by 2 * max_ship_speed_in_system.
    double max_speed_mkm_per_day = 0.0;
    if (swept) {
      for (Id sid : sys->ships) {
        const auto* sh = find_ptr(state_.ships, sid);
        if (!sh) continue;
        const double vx = sh->velocity_mkm_per_day.x;
        const double vy = sh->velocity_mkm_per_day.y;
        const double sp = std::sqrt(vx * vx + vy * vy);
        if (std::isfinite(sp)) max_speed_mkm_per_day = std::max(max_speed_mkm_per_day, sp);
      }
    }
    const double sweep_pad = swept ? (2.0 * max_speed_mkm_per_day * dt_days) : 0.0;

    for (Id fid : faction_ids) {
      const auto& sources = sources_for(fid, sys_id);
      if (sources.empty()) continue;

      for (const auto& src : sources) {
        if (src.range_mkm <= 1e-9) continue;

        const double query_r = src.range_mkm * max_sig + sweep_pad;
        const auto nearby = idx.query_radius(src.pos_mkm, query_r, 1e-9);
        for (Id ship_id : nearby) {
          const auto* sh = find_ptr(state_.ships, ship_id);
          if (!sh) continue;
          if (sh->system_id != sys_id) continue;
          if (sh->faction_id == fid) continue;
          // Apply target signature / EMCON.
          const auto* d = find_design(sh->design_id);
          const double sig = sim_sensors::effective_signature_multiplier(*this, *sh, d);
          const double ecm = d ? std::max(0.0, d->ecm_strength) : 0.0;
          const double eccm = std::max(0.0, src.eccm_strength);
          double ew_mult = (1.0 + eccm) / (1.0 + ecm);
          if (!std::isfinite(ew_mult)) ew_mult = 1.0;
          ew_mult = std::clamp(ew_mult, 0.1, 10.0);

          const double eff = src.range_mkm * sig * ew_mult;
          if (eff <= 1e-9) continue;

          double t_seen = 1.0;

          // If swept detection is enabled, keep endpoints so we can sample
          // positions at t_seen for uncertainty estimation.
          Vec2 tgt0 = sh->position_mkm;
          Vec2 tgt1 = sh->position_mkm;
          Vec2 src0 = src.pos_mkm;
          Vec2 src1 = src.pos_mkm;
          bool have_swept_endpoints = false;

          if (!swept) {
            const double dx = sh->position_mkm.x - src.pos_mkm.x;
            const double dy = sh->position_mkm.y - src.pos_mkm.y;
            if (dx * dx + dy * dy > eff * eff + 1e-9) continue;
          } else {
            tgt1 = sh->position_mkm;
            tgt0 = tgt1 - sh->velocity_mkm_per_day * dt_days;

            src1 = src.pos_mkm;
            src0 = src1;
            if (src.ship_id != kInvalidId) {
              const auto* src_sh = find_ptr(state_.ships, src.ship_id);
              if (src_sh && src_sh->system_id == sys_id) {
                src0 = src1 - src_sh->velocity_mkm_per_day * dt_days;
              }
            }
            have_swept_endpoints = true;

            double t_closest = 0.0;
            const double min_d2 = min_dist_sq_and_t(src0, src1, tgt0, tgt1, &t_closest);
            const double eff2 = eff * eff;
            if (min_d2 > eff2 + 1e-9) continue;

            // Prefer a "last seen" snapshot closer to end-of-tick when the
            // target remains within detection range at the end. Otherwise use
            // the closest-approach time.
            const double dx1 = tgt1.x - src1.x;
            const double dy1 = tgt1.y - src1.y;
            const double d2_end = dx1 * dx1 + dy1 * dy1;
            t_seen = (d2_end <= eff2 + 1e-9) ? 1.0 : t_closest;
          }

          // --- Seed contact uncertainty estimate ---
          double unc_mkm = 0.0;
          if (enable_uncertainty) {
            const double tt = std::clamp(t_seen, 0.0, 1.0);
            const Vec2 srcp = have_swept_endpoints ? (src0 + (src1 - src0) * tt) : src.pos_mkm;
            const Vec2 tgtp = have_swept_endpoints ? (tgt0 + (tgt1 - tgt0) * tt) : sh->position_mkm;
            const Vec2 dd = tgtp - srcp;
            const double d_mkm = std::sqrt(dd.x * dd.x + dd.y * dd.y);
            double frac = unc_frac_lo;
            if (eff > 1e-9) {
              const double u = std::clamp(d_mkm / eff, 0.0, 1.0);
              frac = unc_frac_lo + (unc_frac_hi - unc_frac_lo) * u;
            }
            unc_mkm = std::max(unc_min_mkm, frac * eff);
            if (unc_ecm_mult > 0.0) {
              unc_mkm *= (1.0 + ecm * unc_ecm_mult);
            }
            if (!std::isfinite(unc_mkm) || unc_mkm < 0.0) unc_mkm = 0.0;
            if (std::isfinite(unc_cap_mkm) && unc_cap_mkm > 0.0) {
              unc_mkm = std::min(unc_mkm, unc_cap_mkm);
            }
          }

          detections.push_back(DetectionRecord{ship_id, fid, t_seen, unc_mkm});
        }
      }
    }
  }

  std::sort(detections.begin(), detections.end(), [](const DetectionRecord& a, const DetectionRecord& b) {
    if (a.ship_id != b.ship_id) return a.ship_id < b.ship_id;
    if (a.viewer_faction_id != b.viewer_faction_id) return a.viewer_faction_id < b.viewer_faction_id;
    if (a.t != b.t) return a.t > b.t;
    return a.uncertainty_mkm < b.uncertainty_mkm;
  });
  detections.erase(std::unique(detections.begin(), detections.end(), [](const DetectionRecord& a, const DetectionRecord& b) {
    return a.ship_id == b.ship_id && a.viewer_faction_id == b.viewer_faction_id;
  }), detections.end());

  // Apply today's detections to each faction's contact list.
  for (const auto& det : detections) {
    const auto* sh = find_ptr(state_.ships, det.ship_id);
    if (!sh) continue;

    auto* fac = find_ptr(state_.factions, det.viewer_faction_id);
    if (!fac) continue;
    if (fac->id == sh->faction_id) continue;

    detected_today_by_faction[fac->id].push_back(det.ship_id);

    bool is_new = false;
    bool was_stale = false;
    if (auto it = fac->ship_contacts.find(det.ship_id); it == fac->ship_contacts.end()) {
      is_new = true;
    } else {
      was_stale = (it->second.last_seen_day < now - 1);
    }

    // Update contact memory.
    //
    // We keep a 2-point track (prev/last) to support simple constant-velocity
    // extrapolation for fog-of-war pursuit.
    Contact c;
    if (auto it = fac->ship_contacts.find(det.ship_id); it != fac->ship_contacts.end()) {
      c = it->second;

      // If the contact changed systems since the last detection, reset the
      // previous snapshot (coordinate frame changed).
      if (c.system_id != sh->system_id) {
        c.prev_seen_day = -1;
        c.prev_seen_position_mkm = Vec2{0.0, 0.0};
      } else if (c.last_seen_day >= 0 && c.last_seen_day < now) {
        // Shift last -> prev only once per day, so repeated detections within
        // the same day don't destroy a useful day-over-day velocity estimate.
        c.prev_seen_day = c.last_seen_day;
        c.prev_seen_position_mkm = c.last_seen_position_mkm;
      }
    }

    c.ship_id = det.ship_id;
    c.system_id = sh->system_id;
    c.last_seen_day = now;

    // For swept detections that occurred mid-step, store the interpolated
    // "seen" position so pursuit/prediction has something close to reality.
    const Vec2 end_pos = sh->position_mkm;
    Vec2 start_pos = end_pos;
    if (swept) start_pos = end_pos - sh->velocity_mkm_per_day * dt_days;
    const double tt = std::clamp(det.t, 0.0, 1.0);
    c.last_seen_position_mkm = start_pos + (end_pos - start_pos) * tt;
    c.last_seen_position_uncertainty_mkm = det.uncertainty_mkm;
    if (!std::isfinite(c.last_seen_position_uncertainty_mkm) || c.last_seen_position_uncertainty_mkm < 0.0) {
      c.last_seen_position_uncertainty_mkm = 0.0;
    }
    c.last_seen_name = sh->name;
    c.last_seen_design_id = sh->design_id;
    c.last_seen_faction_id = sh->faction_id;
    fac->ship_contacts[det.ship_id] = std::move(c);

    if (is_new || was_stale) {
      const auto* sys = find_ptr(state_.systems, sh->system_id);
      const std::string sys_name = sys ? sys->name : std::string("(unknown)");
      const auto* other_f = find_ptr(state_.factions, sh->faction_id);
      const std::string other_name = other_f ? other_f->name : std::string("(unknown)");

      EventContext ctx;
      ctx.faction_id = fac->id;
      ctx.faction_id2 = sh->faction_id;
      ctx.system_id = sh->system_id;
      ctx.ship_id = det.ship_id;

      std::string msg;
      if (is_new) {
        msg = "New contact for " + fac->name + ": " + sh->name + " (" + other_name + ") in " + sys_name;
      } else {
        msg = "Contact reacquired for " + fac->name + ": " + sh->name + " (" + other_name + ") in " + sys_name;
      }

      // Don't spam intel events for mutually Friendly factions (allies).
      if (!are_factions_mutual_friendly(fac->id, sh->faction_id)) {
        push_event(EventLevel::Info, EventCategory::Intel, msg, ctx);
      }
    }
  }

  // --- Anomaly discovery (fog-of-war exploration intel) ---
  //
  // Discover unresolved anomalies when they enter any sensor coverage bubble.
  // This reuses the same per-faction sensor source cache as ship contacts.
  const double anom_range_mult = std::clamp(cfg_.anomaly_detection_range_multiplier, 0.0, 100.0);
  if (anom_range_mult > 1e-9 && !state_.anomalies.empty()) {
    std::unordered_map<Id, std::vector<Id>> anomalies_by_system;
    anomalies_by_system.reserve(state_.anomalies.size());

    for (const auto& [aid, a] : state_.anomalies) {
      if (aid == kInvalidId) continue;
      if (a.system_id == kInvalidId) continue;
      if (a.resolved) continue;
      anomalies_by_system[a.system_id].push_back(aid);
    }

    std::vector<Id> anomaly_system_ids;
    anomaly_system_ids.reserve(anomalies_by_system.size());
    for (auto& [sid, vec] : anomalies_by_system) {
      std::sort(vec.begin(), vec.end());
      vec.erase(std::unique(vec.begin(), vec.end()), vec.end());
      anomaly_system_ids.push_back(sid);
    }
    std::sort(anomaly_system_ids.begin(), anomaly_system_ids.end());

    for (Id sys_id : anomaly_system_ids) {
      const auto itv = anomalies_by_system.find(sys_id);
      if (itv == anomalies_by_system.end()) continue;
      const auto& anom_ids = itv->second;

      for (Id fid : faction_ids) {
        const auto& sources = sources_for(fid, sys_id);
        if (sources.empty()) continue;

        for (Id aid : anom_ids) {
          if (is_anomaly_discovered_by_faction(fid, aid)) continue;
          const auto* a = find_ptr(state_.anomalies, aid);
          if (!a || a->resolved) continue;

          const Vec2 tgt = a->position_mkm;
          bool detected_any = false;
          Id discovered_by = kInvalidId;

          for (const auto& src : sources) {
            if (src.range_mkm <= 1e-9) continue;

            const double r = src.range_mkm * anom_range_mult;
            if (!std::isfinite(r) || r <= 1e-9) continue;
            const double r2 = r * r;

            bool detected = false;

            // Swept check for fast-moving ships: did we pass within range at
            // any time during this tick?
            if (swept && src.ship_id != kInvalidId) {
              const auto* sh = find_ptr(state_.ships, src.ship_id);
              const Vec2 src1 = src.pos_mkm;
              Vec2 src0 = src1;
              if (sh) src0 = src1 - sh->velocity_mkm_per_day * dt_days;

              double t = 1.0;
              const double d2 = min_dist_sq_and_t(src0, src1, tgt, tgt, &t);
              detected = (d2 <= r2);
            } else {
              const double dx = tgt.x - src.pos_mkm.x;
              const double dy = tgt.y - src.pos_mkm.y;
              const double d2 = dx * dx + dy * dy;
              detected = (d2 <= r2);
            }

            if (!detected) continue;
            detected_any = true;

            // Prefer the smallest ship_id for deterministic attribution.
            if (src.ship_id != kInvalidId) {
              if (discovered_by == kInvalidId || src.ship_id < discovered_by) discovered_by = src.ship_id;
            }
          }

          if (detected_any) {
            discover_anomaly_for_faction(fid, aid, discovered_by);
          }
        }
      }
    }
  }

  if (!emit_contact_lost_events) return;

  for (Id fid : faction_ids) {
    auto* fac = find_ptr(state_.factions, fid);
    if (!fac) continue;

    auto& detected_today = detected_today_by_faction[fac->id];
    std::sort(detected_today.begin(), detected_today.end());
    detected_today.erase(std::unique(detected_today.begin(), detected_today.end()), detected_today.end());

    std::vector<Id> contact_ship_ids;
    contact_ship_ids.reserve(fac->ship_contacts.size());
    for (const auto& [sid, _] : fac->ship_contacts) contact_ship_ids.push_back(sid);
    std::sort(contact_ship_ids.begin(), contact_ship_ids.end());

    for (Id sid : contact_ship_ids) {
      const auto itc = fac->ship_contacts.find(sid);
      if (itc == fac->ship_contacts.end()) continue;
      const Contact& c = itc->second;

      if (c.last_seen_day != now - 1) continue;
      if (std::binary_search(detected_today.begin(), detected_today.end(), sid)) continue;

      const auto* sys = find_ptr(state_.systems, c.system_id);
      const std::string sys_name = sys ? sys->name : std::string("(unknown)");
      const auto* other_f = find_ptr(state_.factions, c.last_seen_faction_id);
      const std::string other_name = other_f ? other_f->name : std::string("(unknown)");

      EventContext ctx;
      ctx.faction_id = fac->id;
      ctx.faction_id2 = c.last_seen_faction_id;
      ctx.system_id = c.system_id;
      ctx.ship_id = c.ship_id;

      const std::string ship_name = c.last_seen_name.empty() ? ("Ship " + std::to_string(c.ship_id)) : c.last_seen_name;
      const std::string msg = "Contact lost for " + fac->name + ": " + ship_name + " (" + other_name + ") in " + sys_name;

      if (are_factions_mutual_friendly(fac->id, c.last_seen_faction_id)) continue;
      push_event(EventLevel::Info, EventCategory::Intel, msg, ctx);
    }
  }
}

void Simulation::tick_shields(double dt_days) {
  NEBULA4X_TRACE_SCOPE("tick_shields", "sim.combat");
  dt_days = std::clamp(dt_days, 0.0, 10.0);
  const auto ship_ids = sorted_keys(state_.ships);
  for (Id sid : ship_ids) {
    auto* sh = find_ptr(state_.ships, sid);
    if (!sh) continue;
    if (sh->hp <= 0.0) continue;

    const auto* d = find_design(sh->design_id);
    if (!d) {
      // If we can't resolve the design, keep shields at 0 to avoid NaNs.
      sh->shields = 0.0;
      continue;
    }

    const auto p = compute_power_allocation(*d, sh->power_policy);

    const double max_sh = std::max(0.0, d->max_shields);
    const double subsys_mult = ship_subsystem_shield_multiplier(*sh);
    const double max_sh_eff = max_sh * subsys_mult;
    if (max_sh_eff <= 1e-9) {
      sh->shields = 0.0;
      continue;
    }

    // If shields are offline (either due to insufficient power or because
    // the ship's power policy disables them), treat them as fully down.
    if (!p.shields_online) {
      sh->shields = 0.0;
      continue;
    }

    // Initialize shields for older saves / freshly spawned ships.
    if (sh->shields < 0.0) sh->shields = max_sh_eff;

    double regen = std::max(0.0, d->shield_regen_per_day);
    regen *= ship_heat_shield_regen_multiplier(*sh);
    regen *= subsys_mult;

    // Nebula storms can interfere with shield systems (net negative regen).
    double drain = 0.0;
    if (cfg_.enable_nebula_storms) {
      const double per_day = std::max(0.0, cfg_.nebula_storm_shield_drain_per_day_at_intensity1);
      if (per_day > 0.0) {
        const double storm = this->system_storm_intensity(sh->system_id);
        if (storm > 0.0) drain = per_day * storm;
      }
    }

    sh->shields = std::clamp(sh->shields + (regen - drain) * dt_days, 0.0, max_sh_eff);
  }
}


} // namespace nebula4x
