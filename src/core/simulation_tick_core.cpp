#include "nebula4x/core/simulation.h"

#include "simulation_internal.h"

#include "simulation_sensors.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstring>
#include <iomanip>
#include <limits>
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
using sim_internal::compute_power_allocation;

using sim_sensors::SensorSource;
using sim_sensors::gather_sensor_sources;
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
    if (day_advanced) tick_ai();
  } else if (day_advanced) {
    // Daily economy / planning ticks (midnight boundary).
    tick_colonies(1.0, /*emit_daily_events=*/true);
    tick_research(1.0);
    tick_shipyards(1.0);
    tick_construction(1.0);
    tick_ai();
    tick_refuel();
  }

  // Continuous (sub-day) ticks.
  tick_ships(dt_days);
  tick_contacts(day_advanced);
  tick_shields(dt_days);
  if (cfg_.enable_combat) tick_combat(dt_days);

  // Post-movement maintenance / slow processes.
  if (cfg_.enable_subday_economy) {
    tick_refuel();
    tick_terraforming(dt_days);
    tick_repairs(dt_days);
    if (day_advanced) tick_ground_combat();
  } else if (day_advanced) {
    tick_ground_combat();
    tick_terraforming(1.0);
    tick_repairs(1.0);

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

void Simulation::tick_contacts(bool emit_contact_lost_events) {
  NEBULA4X_TRACE_SCOPE("tick_contacts", "sim.sensors");
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
    size_t operator()(const Key& k) const {
      return std::hash<long long>()((static_cast<long long>(k.faction_id) << 32) ^ static_cast<long long>(k.system_id));
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
  // We later sort these pairs by (ship_id, faction_id) to preserve the exact
  // deterministic ordering used by the original nested loop over
  // sorted ship ids then sorted faction ids.
  struct DetectionPair {
    Id ship_id{kInvalidId};
    Id viewer_faction_id{kInvalidId};
  };

  std::vector<DetectionPair> detections;
  detections.reserve(std::min<std::size_t>(state_.ships.size() * 2, 4096));

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

  for (Id sys_id : system_ids) {
    const auto* sys = find_ptr(state_.systems, sys_id);
    if (!sys) continue;
    if (sys->ships.empty()) continue;

    auto& idx = index_for_system(sys_id);

    for (Id fid : faction_ids) {
      const auto& sources = sources_for(fid, sys_id);
      if (sources.empty()) continue;

      for (const auto& src : sources) {
        if (src.range_mkm <= 1e-9) continue;

        const auto nearby = idx.query_radius(src.pos_mkm, src.range_mkm * max_sig, 1e-9);
        for (Id ship_id : nearby) {
          const auto* sh = find_ptr(state_.ships, ship_id);
          if (!sh) continue;
          if (sh->system_id != sys_id) continue;
          if (sh->faction_id == fid) continue;
          // Apply target signature / EMCON.
          const auto* d = find_design(sh->design_id);
          const double sig = sim_sensors::effective_signature_multiplier(*this, *sh, d);
          const double eff = src.range_mkm * sig;
          if (eff <= 1e-9) continue;
          const double dx = sh->position_mkm.x - src.pos_mkm.x;
          const double dy = sh->position_mkm.y - src.pos_mkm.y;
          if (dx * dx + dy * dy > eff * eff + 1e-9) continue;

          detections.push_back(DetectionPair{ship_id, fid});
        }
      }
    }
  }

  std::sort(detections.begin(), detections.end(), [](const DetectionPair& a, const DetectionPair& b) {
    if (a.ship_id != b.ship_id) return a.ship_id < b.ship_id;
    return a.viewer_faction_id < b.viewer_faction_id;
  });
  detections.erase(std::unique(detections.begin(), detections.end(), [](const DetectionPair& a, const DetectionPair& b) {
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
        c.prev_seen_day = 0;
        c.prev_seen_position_mkm = Vec2{0.0, 0.0};
      } else if (c.last_seen_day > 0 && c.last_seen_day < now) {
        // Shift last -> prev only once per day, so repeated detections within
        // the same day don't destroy a useful day-over-day velocity estimate.
        c.prev_seen_day = c.last_seen_day;
        c.prev_seen_position_mkm = c.last_seen_position_mkm;
      }
    }

    c.ship_id = det.ship_id;
    c.system_id = sh->system_id;
    c.last_seen_day = now;
    c.last_seen_position_mkm = sh->position_mkm;
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
    if (max_sh <= 1e-9) {
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
    if (sh->shields < 0.0) sh->shields = max_sh;

    const double regen = std::max(0.0, d->shield_regen_per_day);
    sh->shields = std::clamp(sh->shields + regen * dt_days, 0.0, max_sh);
  }
}


} // namespace nebula4x
