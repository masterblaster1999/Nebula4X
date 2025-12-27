#include "nebula4x/core/simulation.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <queue>
#include <stdexcept>
#include <unordered_map>

#include "nebula4x/core/scenario.h"
#include "nebula4x/util/log.h"

namespace nebula4x {
namespace {

constexpr double kTwoPi = 6.283185307179586476925286766559;

double mkm_per_day_from_speed(double speed_km_s, double seconds_per_day) {
  const double km_per_day = speed_km_s * seconds_per_day;
  return km_per_day / 1.0e6; // million km
}

template <typename T>
void push_unique(std::vector<T>& v, const T& x) {
  if (std::find(v.begin(), v.end(), x) == v.end()) v.push_back(x);
}

bool vec_contains(const std::vector<std::string>& v, const std::string& x) {
  return std::find(v.begin(), v.end(), x) != v.end();
}

bool faction_has_tech(const Faction& f, const std::string& tech_id) {
  return std::find(f.known_techs.begin(), f.known_techs.end(), tech_id) != f.known_techs.end();
}

struct SensorSource {
  Vec2 pos_mkm{0.0, 0.0};
  double range_mkm{0.0};
};

std::vector<SensorSource> gather_sensor_sources(const Simulation& sim, Id faction_id, Id system_id) {
  std::vector<SensorSource> sources;
  const auto& s = sim.state();
  const auto* sys = find_ptr(s.systems, system_id);
  if (!sys) return sources;

  // Friendly ship sensors in this system.
  for (Id sid : sys->ships) {
    const auto* sh = find_ptr(s.ships, sid);
    if (!sh) continue;
    if (sh->faction_id != faction_id) continue;
    const auto* d = sim.find_design(sh->design_id);
    const double range = d ? d->sensor_range_mkm : 0.0;
    if (range <= 0.0) continue;
    sources.push_back(SensorSource{sh->position_mkm, range});
  }

  // Friendly colony-based sensors in this system.
  for (const auto& [_, c] : s.colonies) {
    if (c.faction_id != faction_id) continue;
    const auto* body = find_ptr(s.bodies, c.body_id);
    if (!body || body->system_id != system_id) continue;

    double best = 0.0;
    for (const auto& [inst_id, count] : c.installations) {
      if (count <= 0) continue;
      const auto it = sim.content().installations.find(inst_id);
      if (it == sim.content().installations.end()) continue;
      best = std::max(best, it->second.sensor_range_mkm);
    }

    if (best > 0.0) sources.push_back(SensorSource{body->position_mkm, best});
  }

  return sources;
}

bool any_source_detects(const std::vector<SensorSource>& sources, const Vec2& target_pos) {
  for (const auto& src : sources) {
    if (src.range_mkm <= 0.0) continue;
    const double dist = (target_pos - src.pos_mkm).length();
    if (dist <= src.range_mkm + 1e-9) return true;
  }
  return false;
}

// Many core containers are stored as std::unordered_map for convenience.
// Iteration order of unordered_map is not specified, so relying on it can
// introduce cross-platform nondeterminism (e.g. when multiple ships compete
// for the same resources).
//
// For deterministic simulation ticks, collect keys and process them in a
// stable order.
template <typename Map>
std::vector<typename Map::key_type> sorted_keys(const Map& m) {
  std::vector<typename Map::key_type> keys;
  keys.reserve(m.size());
  for (const auto& [k, _] : m) keys.push_back(k);
  std::sort(keys.begin(), keys.end());
  return keys;
}

} // namespace

Simulation::Simulation(ContentDB content, SimConfig cfg) : content_(std::move(content)), cfg_(cfg) {
  new_game();
}

const ShipDesign* Simulation::find_design(const std::string& design_id) const {
  if (auto it = state_.custom_designs.find(design_id); it != state_.custom_designs.end()) return &it->second;
  if (auto it = content_.designs.find(design_id); it != content_.designs.end()) return &it->second;
  return nullptr;
}

bool Simulation::is_design_buildable_for_faction(Id faction_id, const std::string& design_id) const {
  const auto* d = find_design(design_id);
  if (!d) return false;

  const auto* fac = find_ptr(state_.factions, faction_id);
  if (!fac) return true; // Debug-friendly fallback.

  for (const auto& cid : d->components) {
    if (!vec_contains(fac->unlocked_components, cid)) return false;
  }
  return true;
}

bool Simulation::is_installation_buildable_for_faction(Id faction_id, const std::string& installation_id) const {
  if (content_.installations.find(installation_id) == content_.installations.end()) return false;

  const auto* fac = find_ptr(state_.factions, faction_id);
  if (!fac) return true; // Debug-friendly fallback.

  return vec_contains(fac->unlocked_installations, installation_id);
}

double Simulation::construction_points_per_day(const Colony& colony) const {
  // Baseline: population drives a tiny amount of "free" construction.
  // This keeps the early prototype playable even before dedicated industry is modeled.
  double total = std::max(0.0, colony.population_millions * 0.01);

  for (const auto& [inst_id, count] : colony.installations) {
    if (count <= 0) continue;
    const auto it = content_.installations.find(inst_id);
    if (it == content_.installations.end()) continue;
    const double per_day = it->second.construction_points_per_day;
    if (per_day <= 0.0) continue;
    total += per_day * static_cast<double>(count);
  }

  return total;
}

bool Simulation::is_system_discovered_by_faction(Id viewer_faction_id, Id system_id) const {
  const auto* fac = find_ptr(state_.factions, viewer_faction_id);
  if (!fac) return true; // Debug-friendly fallback.
  return std::find(fac->discovered_systems.begin(), fac->discovered_systems.end(), system_id) !=
         fac->discovered_systems.end();
}

bool Simulation::is_ship_detected_by_faction(Id viewer_faction_id, Id target_ship_id) const {
  const auto* tgt = find_ptr(state_.ships, target_ship_id);
  if (!tgt) return false;
  if (tgt->faction_id == viewer_faction_id) return true;

  const auto sources = gather_sensor_sources(*this, viewer_faction_id, tgt->system_id);
  if (sources.empty()) return false;
  return any_source_detects(sources, tgt->position_mkm);
}

std::vector<Id> Simulation::detected_hostile_ships_in_system(Id viewer_faction_id, Id system_id) const {
  std::vector<Id> out;
  const auto* sys = find_ptr(state_.systems, system_id);
  if (!sys) return out;

  const auto sources = gather_sensor_sources(*this, viewer_faction_id, system_id);
  if (sources.empty()) return out;

  for (Id sid : sys->ships) {
    const auto* sh = find_ptr(state_.ships, sid);
    if (!sh) continue;
    if (sh->faction_id == viewer_faction_id) continue;
    if (any_source_detects(sources, sh->position_mkm)) out.push_back(sid);
  }

  return out;
}

std::vector<Contact> Simulation::recent_contacts_in_system(Id viewer_faction_id, Id system_id, int max_age_days) const {
  std::vector<Contact> out;
  const auto* fac = find_ptr(state_.factions, viewer_faction_id);
  if (!fac) return out;

  const int now = static_cast<int>(state_.date.days_since_epoch());
  for (const auto& [_, c] : fac->ship_contacts) {
    if (c.system_id != system_id) continue;
    const int age = now - c.last_seen_day;
    if (age < 0) continue;
    if (age > max_age_days) continue;
    out.push_back(c);
  }

  std::sort(out.begin(), out.end(), [](const Contact& a, const Contact& b) {
    return a.last_seen_day > b.last_seen_day;
  });
  return out;
}

void Simulation::apply_design_stats_to_ship(Ship& ship) {
  const ShipDesign* d = find_design(ship.design_id);
  if (!d) {
    ship.speed_km_s = 0.0;
    if (ship.hp <= 0.0) ship.hp = 1.0;
    return;
  }

  ship.speed_km_s = d->speed_km_s;
  if (ship.hp <= 0.0) ship.hp = d->max_hp;
  // Clamp just in case content changed between versions.
  ship.hp = std::clamp(ship.hp, 0.0, d->max_hp);
}

bool Simulation::upsert_custom_design(ShipDesign design, std::string* error) {
  auto fail = [&](const std::string& msg) {
    if (error) *error = msg;
    return false;
  };

  if (design.id.empty()) return fail("Design id is empty");
  if (content_.designs.find(design.id) != content_.designs.end()) {
    return fail("Design id conflicts with built-in design: " + design.id);
  }
  if (design.name.empty()) design.name = design.id;

  // Validate components and (re)derive stats.
  double mass = 0.0;
  double speed = 0.0;
  double cargo = 0.0;
  double sensor = 0.0;
  double weapon_damage = 0.0;
  double weapon_range = 0.0;
  double hp_bonus = 0.0;

  for (const auto& cid : design.components) {
    auto it = content_.components.find(cid);
    if (it == content_.components.end()) return fail("Unknown component id: " + cid);
    const auto& c = it->second;

    mass += c.mass_tons;
    speed = std::max(speed, c.speed_km_s);
    cargo += c.cargo_tons;
    sensor = std::max(sensor, c.sensor_range_mkm);

    if (c.type == ComponentType::Weapon) {
      weapon_damage += c.weapon_damage;
      weapon_range = std::max(weapon_range, c.weapon_range_mkm);
    }

    hp_bonus += c.hp_bonus;
  }

  design.mass_tons = mass;
  design.speed_km_s = speed;
  design.cargo_tons = cargo;
  design.sensor_range_mkm = sensor;
  design.weapon_damage = weapon_damage;
  design.weapon_range_mkm = weapon_range;
  design.max_hp = std::max(1.0, mass * 2.0 + hp_bonus);

  state_.custom_designs[design.id] = std::move(design);
  return true;
}

void Simulation::initialize_unlocks_for_faction(Faction& f) {
  // Installations present on colonies belonging to this faction.
  for (Id cid : sorted_keys(state_.colonies)) {
    const auto& col = state_.colonies.at(cid);
    if (col.faction_id != f.id) continue;

    // Exploration: discovering any system where we have a colony.
    if (const auto* body = find_ptr(state_.bodies, col.body_id)) {
      push_unique(f.discovered_systems, body->system_id);
    }

    for (const auto& [inst_id, count] : col.installations) {
      if (count <= 0) continue;
      push_unique(f.unlocked_installations, inst_id);
    }
  }

  // Components present on existing ships belonging to this faction.
  for (Id sid : sorted_keys(state_.ships)) {
    const auto& ship = state_.ships.at(sid);
    if (ship.faction_id != f.id) continue;

    // Exploration: discovering any system where we have a ship.
    push_unique(f.discovered_systems, ship.system_id);

    if (const auto* d = find_design(ship.design_id)) {
      for (const auto& cid : d->components) push_unique(f.unlocked_components, cid);
    }
  }

  // Effects of already-known tech.
  for (const auto& tech_id : f.known_techs) {
    auto tit = content_.techs.find(tech_id);
    if (tit == content_.techs.end()) continue;
    for (const auto& eff : tit->second.effects) {
      if (eff.type == "unlock_component") push_unique(f.unlocked_components, eff.value);
      if (eff.type == "unlock_installation") push_unique(f.unlocked_installations, eff.value);
    }
  }
}

void Simulation::discover_system_for_faction(Id faction_id, Id system_id) {
  if (system_id == kInvalidId) return;
  auto* fac = find_ptr(state_.factions, faction_id);
  if (!fac) return;

  // Only record/log when discovery is new.
  if (std::find(fac->discovered_systems.begin(), fac->discovered_systems.end(), system_id) !=
      fac->discovered_systems.end()) {
    return;
  }

  fac->discovered_systems.push_back(system_id);

  const auto* sys = find_ptr(state_.systems, system_id);
  const std::string sys_name = sys ? sys->name : std::string("(unknown)");

  EventContext ctx;
  ctx.faction_id = faction_id;
  ctx.system_id = system_id;

  const std::string msg = fac->name + " discovered system " + sys_name;
  push_event(EventLevel::Info, EventCategory::Exploration, msg, ctx);
}

void Simulation::new_game() {
  state_ = make_sol_scenario();

  // Apply design stats to ships and seed unlock lists.
  for (auto& [_, ship] : state_.ships) {
    apply_design_stats_to_ship(ship);
  }

  // Seed unlock lists based on starting assets + known tech effects.
  for (auto& [_, f] : state_.factions) initialize_unlocks_for_faction(f);

  recompute_body_positions();

  // Initialize contact memory for the starting situation.
  tick_contacts();
}

void Simulation::load_game(GameState loaded) {
  state_ = std::move(loaded);

  // Ensure the event sequence counter is sane for this save.
  // Older saves may not have had seq numbers; deserialization attempts to
  // assign them, but we still guard here for safety.
  {
    std::uint64_t max_seq = 0;
    for (const auto& ev : state_.events) max_seq = std::max(max_seq, ev.seq);
    if (state_.next_event_seq == 0) state_.next_event_seq = 1;
    if (state_.next_event_seq <= max_seq) state_.next_event_seq = max_seq + 1;
  }

  // Re-derive custom design stats in case the save came from an older version
  // (or content packs changed). We keep any invalid designs but warn.
  if (!state_.custom_designs.empty()) {
    std::vector<ShipDesign> designs;
    designs.reserve(state_.custom_designs.size());
    for (const auto& [_, d] : state_.custom_designs) designs.push_back(d);
    state_.custom_designs.clear();
    for (auto& d : designs) {
      std::string err;
      if (!upsert_custom_design(d, &err)) {
        nebula4x::log::warn(std::string("Custom design '") + d.id + "' could not be re-derived: " + err);
        state_.custom_designs[d.id] = d; // keep as-is
      }
    }
  }

  // Re-derive ship stats in case content changed.
  for (auto& [_, ship] : state_.ships) {
    apply_design_stats_to_ship(ship);
  }

  // Ensure unlock lists include all current ships/colonies + known-tech effects.
  for (auto& [_, f] : state_.factions) {
    initialize_unlocks_for_faction(f);
  }

  recompute_body_positions();

  // Rebuild contact memory for the loaded state (helps older saves).
  tick_contacts();
}

void Simulation::advance_days(int days) {
  if (days <= 0) return;
  for (int i = 0; i < days; ++i) tick_one_day();
}

namespace {

bool event_matches_stop(const SimEvent& ev, const EventStopCondition& stop) {
  const bool level_ok = (ev.level == EventLevel::Info && stop.stop_on_info) ||
                        (ev.level == EventLevel::Warn && stop.stop_on_warn) ||
                        (ev.level == EventLevel::Error && stop.stop_on_error);
  if (!level_ok) return false;

  if (stop.filter_category && ev.category != stop.category) return false;

  if (stop.faction_id != kInvalidId) {
    if (ev.faction_id != stop.faction_id && ev.faction_id2 != stop.faction_id) return false;
  }

  if (!stop.message_contains.empty()) {
    const auto it = std::search(
        ev.message.begin(), ev.message.end(),
        stop.message_contains.begin(), stop.message_contains.end(),
        [](char a, char b) {
          return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
        });
    if (it == ev.message.end()) return false;
  }

  return true;
}

} // namespace

AdvanceUntilEventResult Simulation::advance_until_event(int max_days, const EventStopCondition& stop) {
  AdvanceUntilEventResult out;
  if (max_days <= 0) return out;

  // Track the most recently existing event seq, so we only consider newly
  // recorded events during this time-warp.
  std::uint64_t last_seq = 0;
  if (state_.next_event_seq > 0) last_seq = state_.next_event_seq - 1;

  for (int i = 0; i < max_days; ++i) {
    tick_one_day();
    out.days_advanced += 1;

    const std::uint64_t newest_seq = (state_.next_event_seq > 0) ? (state_.next_event_seq - 1) : 0;
    if (newest_seq <= last_seq) continue; // no new events

    // Scan new events in reverse append order.
    for (int j = static_cast<int>(state_.events.size()) - 1; j >= 0; --j) {
      const auto& ev = state_.events[static_cast<std::size_t>(j)];
      if (ev.seq <= last_seq) break;
      if (!event_matches_stop(ev, stop)) continue;
      out.hit = true;
      out.event = ev; // copy
      return out;
    }

    last_seq = newest_seq;
  }

  return out;
}

bool Simulation::clear_orders(Id ship_id) {
  if (!find_ptr(state_.ships, ship_id)) return false;
  auto& so = state_.ship_orders[ship_id];
  so.queue.clear();
  so.repeat = false;
  so.repeat_template.clear();
  return true;
}

bool Simulation::enable_order_repeat(Id ship_id) {
  if (!find_ptr(state_.ships, ship_id)) return false;
  auto& so = state_.ship_orders[ship_id];
  if (so.queue.empty()) return false;
  so.repeat = true;
  so.repeat_template = so.queue;
  return true;
}

bool Simulation::update_order_repeat_template(Id ship_id) {
  if (!find_ptr(state_.ships, ship_id)) return false;
  auto& so = state_.ship_orders[ship_id];
  if (so.queue.empty()) return false;
  so.repeat = true;
  so.repeat_template = so.queue;
  return true;
}

bool Simulation::disable_order_repeat(Id ship_id) {
  if (!find_ptr(state_.ships, ship_id)) return false;
  auto& so = state_.ship_orders[ship_id];
  so.repeat = false;
  so.repeat_template.clear();
  return true;
}

bool Simulation::cancel_current_order(Id ship_id) {
  if (!find_ptr(state_.ships, ship_id)) return false;
  auto it = state_.ship_orders.find(ship_id);
  if (it == state_.ship_orders.end() || it->second.queue.empty()) return false;
  it->second.queue.erase(it->second.queue.begin());
  return true;
}

bool Simulation::issue_wait_days(Id ship_id, int days) {
  if (days <= 0) return false;
  if (!find_ptr(state_.ships, ship_id)) return false;
  auto& orders = state_.ship_orders[ship_id];
  orders.queue.push_back(WaitDays{days});
  return true;
}

bool Simulation::issue_move_to_point(Id ship_id, Vec2 target_mkm) {
  auto* ship = find_ptr(state_.ships, ship_id);
  if (!ship) return false;
  auto& orders = state_.ship_orders[ship_id];
  orders.queue.push_back(MoveToPoint{target_mkm});
  return true;
}

bool Simulation::issue_move_to_body(Id ship_id, Id body_id, bool restrict_to_discovered) {
  auto* ship = find_ptr(state_.ships, ship_id);
  if (!ship) return false;
  const auto* body = find_ptr(state_.bodies, body_id);
  if (!body) return false;

  const Id target_system_id = body->system_id;
  if (target_system_id == kInvalidId) return false;
  if (!find_ptr(state_.systems, target_system_id)) return false;

  // Route (if needed) so that when this order reaches the front of the queue,
  // the ship will already be in the correct system.
  if (!issue_travel_to_system(ship_id, target_system_id, restrict_to_discovered)) return false;

  auto& orders = state_.ship_orders[ship_id];
  orders.queue.push_back(MoveToBody{body_id});
  return true;
}

bool Simulation::issue_travel_via_jump(Id ship_id, Id jump_point_id) {
  auto* ship = find_ptr(state_.ships, ship_id);
  if (!ship) return false;
  if (!find_ptr(state_.jump_points, jump_point_id)) return false;
  auto& orders = state_.ship_orders[ship_id];
  orders.queue.push_back(TravelViaJump{jump_point_id});
  return true;
}

bool Simulation::issue_travel_to_system(Id ship_id, Id target_system_id, bool restrict_to_discovered) {
  auto* ship = find_ptr(state_.ships, ship_id);
  if (!ship) return false;
  if (!find_ptr(state_.systems, target_system_id)) return false;

  // When queuing travel routes, treat the ship's "current" system as the system
  // it will be in after executing any already-queued TravelViaJump orders.
  // This makes Shift-queued travel routes behave intuitively.
  auto predicted_system_after_queue = [&]() -> Id {
    Id sys = ship->system_id;
    auto it = state_.ship_orders.find(ship_id);
    if (it == state_.ship_orders.end()) return sys;

    for (const auto& ord : it->second.queue) {
      if (!std::holds_alternative<TravelViaJump>(ord)) continue;
      const Id jump_id = std::get<TravelViaJump>(ord).jump_point_id;
      const auto* jp = find_ptr(state_.jump_points, jump_id);
      if (!jp) continue;
      if (jp->system_id != sys) continue;
      if (jp->linked_jump_id == kInvalidId) continue;
      const auto* dest = find_ptr(state_.jump_points, jp->linked_jump_id);
      if (!dest) continue;
      if (dest->system_id == kInvalidId) continue;
      if (!find_ptr(state_.systems, dest->system_id)) continue;
      sys = dest->system_id;
    }
    return sys;
  };

  const Id start = predicted_system_after_queue();
  if (start == kInvalidId) return false;
  if (start == target_system_id) return true; // no-op

  // Breadth-first search over the system graph, tracking the jump id used to traverse each edge.

  auto allow_system = [&](Id sys_id) {
    if (!restrict_to_discovered) return true;
    return is_system_discovered_by_faction(ship->faction_id, sys_id);
  };

  // If the caller wants discovery-restricted routing, the destination must also be discovered.
  if (restrict_to_discovered && !allow_system(target_system_id)) return false;

  std::queue<Id> q;
  std::unordered_map<Id, Id> prev_system;
  std::unordered_map<Id, Id> prev_jump;

  q.push(start);
  prev_system[start] = kInvalidId;

  while (!q.empty()) {
    const Id cur = q.front();
    q.pop();
    if (cur == target_system_id) break;

    const auto* sys = find_ptr(state_.systems, cur);
    if (!sys) continue;

    for (Id jid : sys->jump_points) {
      const auto* jp = find_ptr(state_.jump_points, jid);
      if (!jp) continue;
      if (jp->linked_jump_id == kInvalidId) continue;

      const auto* dest_jp = find_ptr(state_.jump_points, jp->linked_jump_id);
      if (!dest_jp) continue;

      const Id next_sys = dest_jp->system_id;
      if (next_sys == kInvalidId) continue;
      if (!find_ptr(state_.systems, next_sys)) continue;
      if (restrict_to_discovered && !allow_system(next_sys)) continue;
      if (prev_system.find(next_sys) != prev_system.end()) continue; // visited

      prev_system[next_sys] = cur;
      prev_jump[next_sys] = jid;
      q.push(next_sys);
    }
  }

  if (prev_system.find(target_system_id) == prev_system.end()) return false;

  std::vector<Id> jumps;
  for (Id cur = target_system_id; cur != start;) {
    auto it_sys = prev_system.find(cur);
    auto it_jump = prev_jump.find(cur);
    if (it_sys == prev_system.end() || it_jump == prev_jump.end()) return false;
    jumps.push_back(it_jump->second);
    cur = it_sys->second;
  }
  std::reverse(jumps.begin(), jumps.end());
  auto& orders = state_.ship_orders[ship_id];
  for (Id jid : jumps) orders.queue.push_back(TravelViaJump{jid});
  return true;
}

bool Simulation::issue_attack_ship(Id attacker_ship_id, Id target_ship_id, bool restrict_to_discovered) {
  if (attacker_ship_id == target_ship_id) return false;
  auto* attacker = find_ptr(state_.ships, attacker_ship_id);
  if (!attacker) return false;
  const auto* target = find_ptr(state_.ships, target_ship_id);
  if (!target) return false;
  if (target->faction_id == attacker->faction_id) return false;

  // Sensor gating / intel-based targeting:
  // - If the target is currently detected (by this faction, anywhere we have sensors), record its true position.
  // - Otherwise, allow issuing an intercept if we have a contact snapshot (last-seen system + position).
  const bool detected = is_ship_detected_by_faction(attacker->faction_id, target_ship_id);

  AttackShip ord;
  ord.target_ship_id = target_ship_id;

  Id target_system_id = kInvalidId;

  if (detected) {
    ord.has_last_known = true;
    ord.last_known_position_mkm = target->position_mkm;
    target_system_id = target->system_id;
  } else {
    const auto* fac = find_ptr(state_.factions, attacker->faction_id);
    if (!fac) return false;
    const auto it = fac->ship_contacts.find(target_ship_id);
    if (it == fac->ship_contacts.end()) return false;
    ord.has_last_known = true;
    ord.last_known_position_mkm = it->second.last_seen_position_mkm;
    target_system_id = it->second.system_id;
  }

  if (target_system_id == kInvalidId) return false;
  if (!find_ptr(state_.systems, target_system_id)) return false;

  // Auto-route across systems so that when the attack order reaches the front of the queue,
  // the ship will already be in the same system as the target (or last-known target system).
  if (!issue_travel_to_system(attacker_ship_id, target_system_id, restrict_to_discovered)) return false;

  auto& orders = state_.ship_orders[attacker_ship_id];
  orders.queue.push_back(ord);
  return true;
}

bool Simulation::issue_load_mineral(Id ship_id, Id colony_id, const std::string& mineral, double tons,
                                    bool restrict_to_discovered) {
  auto* ship = find_ptr(state_.ships, ship_id);
  if (!ship) return false;
  auto* colony = find_ptr(state_.colonies, colony_id);
  if (!colony) return false;
  if (colony->faction_id != ship->faction_id) return false;
  const auto* body = find_ptr(state_.bodies, colony->body_id);
  if (!body) return false;
  if (body->system_id == kInvalidId) return false;
  if (!find_ptr(state_.systems, body->system_id)) return false;
  if (tons < 0.0) return false;

  if (!issue_travel_to_system(ship_id, body->system_id, restrict_to_discovered)) return false;

  auto& orders = state_.ship_orders[ship_id];
  orders.queue.push_back(LoadMineral{colony_id, mineral, tons});
  return true;
}

bool Simulation::issue_unload_mineral(Id ship_id, Id colony_id, const std::string& mineral, double tons,
                                      bool restrict_to_discovered) {
  auto* ship = find_ptr(state_.ships, ship_id);
  if (!ship) return false;
  auto* colony = find_ptr(state_.colonies, colony_id);
  if (!colony) return false;
  if (colony->faction_id != ship->faction_id) return false;
  const auto* body = find_ptr(state_.bodies, colony->body_id);
  if (!body) return false;
  if (body->system_id == kInvalidId) return false;
  if (!find_ptr(state_.systems, body->system_id)) return false;
  if (tons < 0.0) return false;

  if (!issue_travel_to_system(ship_id, body->system_id, restrict_to_discovered)) return false;

  auto& orders = state_.ship_orders[ship_id];
  orders.queue.push_back(UnloadMineral{colony_id, mineral, tons});
  return true;
}

bool Simulation::enqueue_build(Id colony_id, const std::string& design_id) {
  auto* colony = find_ptr(state_.colonies, colony_id);
  if (!colony) return false;
  const auto it_yard = colony->installations.find("shipyard");
  if (it_yard == colony->installations.end() || it_yard->second <= 0) return false;
  const auto* d = find_design(design_id);
  if (!d) return false;
  if (!is_design_buildable_for_faction(colony->faction_id, design_id)) return false;
  BuildOrder bo;
  bo.design_id = design_id;
  bo.tons_remaining = std::max(1.0, d->mass_tons);
  colony->shipyard_queue.push_back(bo);
  return true;
}

bool Simulation::enqueue_installation_build(Id colony_id, const std::string& installation_id, int quantity) {
  auto* colony = find_ptr(state_.colonies, colony_id);
  if (!colony) return false;
  if (quantity <= 0) return false;
  if (content_.installations.find(installation_id) == content_.installations.end()) return false;
  if (!is_installation_buildable_for_faction(colony->faction_id, installation_id)) return false;

  InstallationBuildOrder o;
  o.installation_id = installation_id;
  o.quantity_remaining = quantity;
  colony->construction_queue.push_back(o);
  return true;
}

void Simulation::recompute_body_positions() {
  const double t = static_cast<double>(state_.date.days_since_epoch());
  for (auto& [_, b] : state_.bodies) {
    if (b.orbit_radius_mkm <= 1e-9) {
      b.position_mkm = {0.0, 0.0};
      continue;
    }
    const double period = std::max(1.0, b.orbit_period_days);
    const double theta = b.orbit_phase_radians + kTwoPi * (t / period);
    b.position_mkm = {b.orbit_radius_mkm * std::cos(theta), b.orbit_radius_mkm * std::sin(theta)};
  }
}

void Simulation::tick_one_day() {
  state_.date = state_.date.add_days(1);
  recompute_body_positions();
  tick_colonies();
  tick_research();
  tick_shipyards();
  tick_construction();
  tick_ships();
  tick_contacts();
  tick_combat();
}

void Simulation::push_event(EventLevel level, std::string message) {
  push_event(level, EventCategory::General, std::move(message), {});
}

void Simulation::push_event(EventLevel level, EventCategory category, std::string message, EventContext ctx) {
  SimEvent ev;
  // Assign a monotonic sequence id (persisted in the save).
  ev.seq = state_.next_event_seq;
  state_.next_event_seq += 1;
  if (state_.next_event_seq == 0) state_.next_event_seq = 1; // ultra-paranoid wrap guard

  ev.day = state_.date.days_since_epoch();
  ev.level = level;
  ev.category = category;
  ev.faction_id = ctx.faction_id;
  ev.faction_id2 = ctx.faction_id2;
  ev.system_id = ctx.system_id;
  ev.ship_id = ctx.ship_id;
  ev.colony_id = ctx.colony_id;
  ev.message = std::move(message);
  state_.events.push_back(std::move(ev));

  // Prune old events occasionally (amortized) to avoid unbounded save growth.
  const int max_events = cfg_.max_events;
  if (max_events > 0 && static_cast<int>(state_.events.size()) > max_events + 128) {
    const std::size_t keep = static_cast<std::size_t>(max_events);
    const std::size_t cut = state_.events.size() - keep;
    state_.events.erase(state_.events.begin(), state_.events.begin() + static_cast<std::ptrdiff_t>(cut));
  }
}



void Simulation::tick_contacts() {
  // Very simple intel model:
  // - If a hostile ship is detected, store a snapshot as a Contact.
  // - Contacts are retained for a while even after losing contact.

  const int now = static_cast<int>(state_.date.days_since_epoch());
  constexpr int kMaxContactAgeDays = 180;

  // Prune obviously invalid / very old contacts.
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

  // Cache sensor sources per (faction, system) to avoid re-scanning colonies repeatedly.
  struct Key {
    Id faction_id{kInvalidId};
    Id system_id{kInvalidId};
    bool operator==(const Key& o) const { return faction_id == o.faction_id && system_id == o.system_id; }
  };
  struct KeyHash {
    size_t operator()(const Key& k) const {
      // Cheap mixing; ids are small in the prototype.
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

  // Track which hostile ships were detected today per faction.
  // Used to generate one-shot "contact lost" events.
  std::unordered_map<Id, std::vector<Id>> detected_today_by_faction;
  detected_today_by_faction.reserve(state_.factions.size());

  // Deterministic: iterate ships + factions in stable id order.
  const auto ship_ids = sorted_keys(state_.ships);
  const auto faction_ids = sorted_keys(state_.factions);

  for (Id ship_id : ship_ids) {
    const auto* sh = find_ptr(state_.ships, ship_id);
    if (!sh) continue;

    for (Id fid : faction_ids) {
      auto* fac = find_ptr(state_.factions, fid);
      if (!fac) continue;
      if (fac->id == sh->faction_id) continue;

      const auto& sources = sources_for(fac->id, sh->system_id);
      if (sources.empty()) continue;
      if (!any_source_detects(sources, sh->position_mkm)) continue;

      // Record that this ship was detected today (for later "lost contact" detection).
      detected_today_by_faction[fac->id].push_back(ship_id);

      // Determine whether this is a new contact or a reacquisition.
      bool is_new = false;
      bool was_stale = false;
      if (auto it = fac->ship_contacts.find(ship_id); it == fac->ship_contacts.end()) {
        is_new = true;
      } else {
        was_stale = (it->second.last_seen_day < now - 1);
      }

      Contact c;
      c.ship_id = ship_id;
      c.system_id = sh->system_id;
      c.last_seen_day = now;
      c.last_seen_position_mkm = sh->position_mkm;
      c.last_seen_name = sh->name;
      c.last_seen_design_id = sh->design_id;
      c.last_seen_faction_id = sh->faction_id;
      fac->ship_contacts[ship_id] = std::move(c);

      if (is_new || was_stale) {
        const auto* sys = find_ptr(state_.systems, sh->system_id);
        const std::string sys_name = sys ? sys->name : std::string("(unknown)");

        const auto* other_f = find_ptr(state_.factions, sh->faction_id);
        const std::string other_name = other_f ? other_f->name : std::string("(unknown)");

        EventContext ctx;
        ctx.faction_id = fac->id;
        ctx.faction_id2 = sh->faction_id;
        ctx.system_id = sh->system_id;
        ctx.ship_id = ship_id;

        std::string msg;
        if (is_new) {
          msg = "New contact for " + fac->name + ": " + sh->name + " (" + other_name + ") in " + sys_name;
        } else {
          msg = "Contact reacquired for " + fac->name + ": " + sh->name + " (" + other_name + ") in " + sys_name;
        }

        push_event(EventLevel::Info, EventCategory::Intel, msg, ctx);
      }
    }
  }

  // Contact lost events: if a contact was seen yesterday but not today, record a one-shot info event.
  for (Id fid : faction_ids) {
    auto* fac = find_ptr(state_.factions, fid);
    if (!fac) continue;

    auto& detected_today = detected_today_by_faction[fac->id];
    std::sort(detected_today.begin(), detected_today.end());
    detected_today.erase(std::unique(detected_today.begin(), detected_today.end()), detected_today.end());

    // Deterministic: iterate contacts in stable ship-id order.
    std::vector<Id> contact_ship_ids;
    contact_ship_ids.reserve(fac->ship_contacts.size());
    for (const auto& [sid, _] : fac->ship_contacts) contact_ship_ids.push_back(sid);
    std::sort(contact_ship_ids.begin(), contact_ship_ids.end());

    for (Id sid : contact_ship_ids) {
      const auto itc = fac->ship_contacts.find(sid);
      if (itc == fac->ship_contacts.end()) continue;
      const Contact& c = itc->second;

      // Only emit "lost" when it was detected yesterday.
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

      push_event(EventLevel::Info, EventCategory::Intel, msg, ctx);
    }
  }
}

void Simulation::tick_colonies() {
  // Deterministic: iterate colonies in stable id order.
  for (Id cid : sorted_keys(state_.colonies)) {
    auto& colony = state_.colonies.at(cid);
    for (const auto& [inst_id, count] : colony.installations) {
      if (count <= 0) continue;
      auto dit = content_.installations.find(inst_id);
      if (dit == content_.installations.end()) continue;
      for (const auto& [mineral, per_day] : dit->second.produces_per_day) {
        colony.minerals[mineral] += per_day * static_cast<double>(count);
      }
    }
  }
}

void Simulation::tick_research() {
  // Generate RP from colonies.
  // Deterministic: iterate colonies in stable id order.
  for (Id cid : sorted_keys(state_.colonies)) {
    auto& col = state_.colonies.at(cid);
    // Look for research labs.
    double rp_per_day = 0.0;
    for (const auto& [inst_id, count] : col.installations) {
      const auto dit = content_.installations.find(inst_id);
      if (dit == content_.installations.end()) continue;
      rp_per_day += dit->second.research_points_per_day * static_cast<double>(count);
    }
    if (rp_per_day <= 0.0) continue;
    auto fit = state_.factions.find(col.faction_id);
    if (fit == state_.factions.end()) continue;
    fit->second.research_points += rp_per_day;
  }

  auto prereqs_met = [&](const Faction& f, const TechDef& t) {
    for (const auto& p : t.prereqs) {
      if (!faction_has_tech(f, p)) return false;
    }
    return true;
  };

  // Spend RP in each faction.
  // Deterministic: iterate factions in stable id order.
  for (Id fid : sorted_keys(state_.factions)) {
    auto& fac = state_.factions.at(fid);
    auto enqueue_unique = [&](const std::string& tech_id) {
      if (tech_id.empty()) return;
      if (faction_has_tech(fac, tech_id)) return;
      if (std::find(fac.research_queue.begin(), fac.research_queue.end(), tech_id) != fac.research_queue.end()) return;
      fac.research_queue.push_back(tech_id);
    };

    // Remove invalid or already-known tech IDs from the queue.
    auto clean_queue = [&]() {
      auto keep = [&](const std::string& id) {
        if (id.empty()) return false;
        if (faction_has_tech(fac, id)) return false;
        const bool ok = (content_.techs.find(id) != content_.techs.end());
        if (!ok && !content_.techs.empty()) {
          log::warn("Unknown tech in research queue: " + id);
        }
        return ok;
      };
      fac.research_queue.erase(std::remove_if(fac.research_queue.begin(), fac.research_queue.end(),
                                             [&](const std::string& id) { return !keep(id); }),
                               fac.research_queue.end());
    };

    // Pick the next research item whose prereqs are satisfied (scan the full queue).
    auto select_next_available = [&]() {
      clean_queue();
      fac.active_research_id.clear();
      fac.active_research_progress = 0.0;

      for (std::size_t i = 0; i < fac.research_queue.size(); ++i) {
        const std::string& id = fac.research_queue[i];
        const auto it = content_.techs.find(id);
        if (it == content_.techs.end()) continue;
        if (!prereqs_met(fac, it->second)) continue;

        fac.active_research_id = id;
        fac.active_research_progress = 0.0;
        fac.research_queue.erase(fac.research_queue.begin() + static_cast<std::ptrdiff_t>(i));
        return;
      }
    };

    // Validate active research (can be set via UI or loaded saves).
    if (!fac.active_research_id.empty()) {
      if (faction_has_tech(fac, fac.active_research_id)) {
        // Already researched; clear.
        fac.active_research_id.clear();
        fac.active_research_progress = 0.0;
      } else {
        const auto it = content_.techs.find(fac.active_research_id);
        if (it == content_.techs.end()) {
          if (!content_.techs.empty()) {
            log::warn("Unknown active research tech: " + fac.active_research_id);
          }
          fac.active_research_id.clear();
          fac.active_research_progress = 0.0;
        } else if (!prereqs_met(fac, it->second)) {
          // Don't deadlock the research system: if active research is blocked by prereqs,
          // move it back into the queue and pick something else.
          enqueue_unique(fac.active_research_id);
          fac.active_research_id.clear();
          fac.active_research_progress = 0.0;
        }
      }
    }

    if (fac.active_research_id.empty()) select_next_available();

    // Keep consuming RP and completing projects in this faction until we either
    // run out of RP or have nothing available to research.
    for (;;) {
      if (fac.active_research_id.empty()) break;

      const auto it2 = content_.techs.find(fac.active_research_id);
      if (it2 == content_.techs.end()) {
        // Shouldn't happen due to validation/cleaning, but keep it robust.
        fac.active_research_id.clear();
        fac.active_research_progress = 0.0;
        select_next_available();
        continue;
      }

      const TechDef& tech = it2->second;

      if (faction_has_tech(fac, tech.id)) {
        // Already known (possible after loading a save with duplicates). Skip.
        fac.active_research_id.clear();
        fac.active_research_progress = 0.0;
        select_next_available();
        continue;
      }

      if (!prereqs_met(fac, tech)) {
        // Prereqs missing: requeue and try something else.
        enqueue_unique(tech.id);
        fac.active_research_id.clear();
        fac.active_research_progress = 0.0;
        select_next_available();
        continue;
      }

      const double remaining = std::max(0.0, tech.cost - fac.active_research_progress);

      // Complete (even if no RP remains this tick).
      if (remaining <= 0.0) {
        fac.known_techs.push_back(tech.id);

        // Apply effects (unlock lists).
        for (const auto& eff : tech.effects) {
          if (eff.type == "unlock_component") {
            push_unique(fac.unlocked_components, eff.value);
          } else if (eff.type == "unlock_installation") {
            push_unique(fac.unlocked_installations, eff.value);
          }
        }

        {
          const std::string msg = "Research complete for " + fac.name + ": " + tech.name;
          log::info(msg);
          EventContext ctx;
          ctx.faction_id = fac.id;
          push_event(EventLevel::Info, EventCategory::Research, msg, ctx);
        }

        fac.active_research_id.clear();
        fac.active_research_progress = 0.0;
        select_next_available();
        continue;
      }

      // No RP left to apply today.
      if (fac.research_points <= 0.0) break;

      // Spend.
      const double spend = std::min(fac.research_points, remaining);
      fac.research_points -= spend;
      fac.active_research_progress += spend;
    }
  }
}


void Simulation::tick_shipyards() {
  const auto it_def = content_.installations.find("shipyard");
  if (it_def == content_.installations.end()) return;

  const InstallationDef& shipyard_def = it_def->second;
  const double base_rate = shipyard_def.build_rate_tons_per_day;
  if (base_rate <= 0.0) return;

  const auto& costs_per_ton = shipyard_def.build_costs_per_ton;

  auto max_build_by_minerals = [&](const Colony& colony, double desired_tons) {
    double max_tons = desired_tons;
    for (const auto& [mineral, cost_per_ton] : costs_per_ton) {
      if (cost_per_ton <= 0.0) continue;
      const auto it = colony.minerals.find(mineral);
      const double available = (it == colony.minerals.end()) ? 0.0 : it->second;
      max_tons = std::min(max_tons, available / cost_per_ton);
    }
    return max_tons;
  };

  auto consume_minerals = [&](Colony& colony, double built_tons) {
    for (const auto& [mineral, cost_per_ton] : costs_per_ton) {
      if (cost_per_ton <= 0.0) continue;
      const double cost = built_tons * cost_per_ton;
      colony.minerals[mineral] = std::max(0.0, colony.minerals[mineral] - cost);
    }
  };

  // Deterministic: iterate colonies in stable id order.
  for (Id cid : sorted_keys(state_.colonies)) {
    auto& colony = state_.colonies.at(cid);
    const auto it_yard = colony.installations.find("shipyard");
    const int yards = (it_yard != colony.installations.end()) ? it_yard->second : 0;
    if (yards <= 0) continue;

    double capacity_tons = base_rate * static_cast<double>(yards);

    while (capacity_tons > 1e-9 && !colony.shipyard_queue.empty()) {
      auto& bo = colony.shipyard_queue.front();

      double build_tons = std::min(capacity_tons, bo.tons_remaining);

      // Apply mineral constraints (if costs are configured).
      if (!costs_per_ton.empty()) {
        build_tons = max_build_by_minerals(colony, build_tons);
      }

      if (build_tons <= 1e-9) {
        // Stalled due to lack of minerals (or zero capacity).
        break;
      }

      // Spend minerals and progress the build.
      if (!costs_per_ton.empty()) consume_minerals(colony, build_tons);
      bo.tons_remaining -= build_tons;
      capacity_tons -= build_tons;

      if (bo.tons_remaining > 1e-9) {
        // Not finished; all remaining capacity (if any) will be unused this day.
        break;
      }

      // Build complete: spawn ship at colony body position.
      const auto* design = find_design(bo.design_id);
      if (!design) {
        {
          const std::string msg = std::string("Unknown design in build queue: ") + bo.design_id;
          nebula4x::log::warn(msg);
          EventContext ctx;
          ctx.faction_id = colony.faction_id;
          ctx.colony_id = colony.id;
          push_event(EventLevel::Warn, EventCategory::Shipyard, msg, ctx);
        }
      } else {
        const auto* body = find_ptr(state_.bodies, colony.body_id);
        if (!body) {
          {
            const std::string msg = "Colony " + std::to_string(colony.id) + " has missing body " +
                                    std::to_string(colony.body_id);
            nebula4x::log::warn(msg);
            EventContext ctx;
            ctx.faction_id = colony.faction_id;
            ctx.colony_id = colony.id;
            push_event(EventLevel::Warn, EventCategory::Shipyard, msg, ctx);
          }
        } else {
          const auto* sys = find_ptr(state_.systems, body->system_id);
          if (!sys) {
            {
              const std::string msg = "Body " + std::to_string(body->id) + " has missing system " +
                                      std::to_string(body->system_id);
              nebula4x::log::warn(msg);
              EventContext ctx;
              ctx.faction_id = colony.faction_id;
              ctx.colony_id = colony.id;
              ctx.system_id = body->system_id;
              push_event(EventLevel::Warn, EventCategory::Shipyard, msg, ctx);
            }
          } else {
            Ship sh;
            sh.id = allocate_id(state_);
            sh.faction_id = colony.faction_id;
            sh.system_id = body->system_id;
            sh.design_id = bo.design_id;
            sh.position_mkm = body->position_mkm;

            apply_design_stats_to_ship(sh);

            // Simple name numbering.
            sh.name = design->name + " #" + std::to_string(sh.id);

            state_.ships[sh.id] = sh;
            state_.ship_orders[sh.id] = ShipOrders{};
            state_.systems[sh.system_id].ships.push_back(sh.id);

            {
              const std::string msg = "Built ship " + sh.name + " (" + sh.design_id + ") at " + colony.name;
              nebula4x::log::info(msg);
              EventContext ctx;
              ctx.faction_id = colony.faction_id;
              ctx.system_id = sh.system_id;
              ctx.ship_id = sh.id;
              ctx.colony_id = colony.id;
              push_event(EventLevel::Info, EventCategory::Shipyard, msg, ctx);
            }
          }
        }
      }

      colony.shipyard_queue.erase(colony.shipyard_queue.begin());
    }
  }
}

void Simulation::tick_construction() {
  // Deterministic: iterate colonies in stable id order.
  for (Id cid : sorted_keys(state_.colonies)) {
    auto& colony = state_.colonies.at(cid);

    Id colony_system_id = kInvalidId;
    if (auto* b = find_ptr(state_.bodies, colony.body_id)) colony_system_id = b->system_id;
    // Cache CP/day at the start of the tick so newly completed factories don't
    // immediately grant extra CP within the same day.
    double cp_available = construction_points_per_day(colony);
    if (cp_available <= 1e-9) continue;

    auto can_pay_minerals = [&](const InstallationDef& def) {
      for (const auto& [mineral, cost] : def.build_costs) {
        if (cost <= 0.0) continue;
        const auto it = colony.minerals.find(mineral);
        const double have = (it == colony.minerals.end()) ? 0.0 : it->second;
        if (have + 1e-9 < cost) return false;
      }
      return true;
    };

    auto pay_minerals = [&](const InstallationDef& def) {
      for (const auto& [mineral, cost] : def.build_costs) {
        if (cost <= 0.0) continue;
        colony.minerals[mineral] = std::max(0.0, colony.minerals[mineral] - cost);
      }
    };

    while (cp_available > 1e-9 && !colony.construction_queue.empty()) {
      auto& ord = colony.construction_queue.front();

      if (ord.quantity_remaining <= 0) {
        colony.construction_queue.erase(colony.construction_queue.begin());
        continue;
      }

      auto it_def = content_.installations.find(ord.installation_id);
      if (it_def == content_.installations.end()) {
        {
          const std::string msg = "Unknown installation in construction queue: " + ord.installation_id;
          nebula4x::log::warn(msg);
          EventContext ctx;
          ctx.faction_id = colony.faction_id;
          ctx.system_id = colony_system_id;
          ctx.colony_id = colony.id;
          push_event(EventLevel::Warn, EventCategory::Construction, msg, ctx);
        }
        colony.construction_queue.erase(colony.construction_queue.begin());
        continue;
      }
      const InstallationDef& def = it_def->second;

      // If we haven't started the current unit yet, attempt to pay minerals.
      if (!ord.minerals_paid) {
        if (!can_pay_minerals(def)) {
          // Stalled due to missing minerals.
          break;
        }
        pay_minerals(def);
        ord.minerals_paid = true;
        ord.cp_remaining = std::max(0.0, def.construction_cost);

        // Instant-build installations (cost 0 CP) complete immediately.
        if (ord.cp_remaining <= 1e-9) {
          colony.installations[def.id] += 1;
          ord.quantity_remaining -= 1;
          ord.minerals_paid = false;
          ord.cp_remaining = 0.0;

          {
            const std::string msg = "Constructed " + def.name + " at " + colony.name;
            nebula4x::log::info(msg);
            EventContext ctx;
            ctx.faction_id = colony.faction_id;
            ctx.system_id = colony_system_id;
            ctx.colony_id = colony.id;
            push_event(EventLevel::Info, EventCategory::Construction, msg, ctx);
          }

          if (ord.quantity_remaining <= 0) {
            colony.construction_queue.erase(colony.construction_queue.begin());
          }
          continue;
        }
      }

      // Spend construction points.
      const double spend = std::min(cp_available, ord.cp_remaining);
      ord.cp_remaining -= spend;
      cp_available -= spend;

      if (ord.cp_remaining <= 1e-9) {
        colony.installations[def.id] += 1;
        ord.quantity_remaining -= 1;
        ord.minerals_paid = false;
        ord.cp_remaining = 0.0;

        {
          const std::string msg = "Constructed " + def.name + " at " + colony.name;
          nebula4x::log::info(msg);
          EventContext ctx;
          ctx.faction_id = colony.faction_id;
          ctx.system_id = colony_system_id;
          ctx.colony_id = colony.id;
          push_event(EventLevel::Info, EventCategory::Construction, msg, ctx);
        }

        if (ord.quantity_remaining <= 0) {
          colony.construction_queue.erase(colony.construction_queue.begin());
        }
        continue;
      }

      // Not complete; no more CP remains (spend used the remaining CP).
      break;
    }
  }
}

void Simulation::tick_ships() {
  auto cargo_used_tons = [](const Ship& s) {
    double used = 0.0;
    for (const auto& [_, tons] : s.cargo) used += std::max(0.0, tons);
    return used;
  };

  const auto ship_ids = sorted_keys(state_.ships);
  for (Id ship_id : ship_ids) {
    auto it_ship = state_.ships.find(ship_id);
    if (it_ship == state_.ships.end()) continue;
    auto& ship = it_ship->second;

    auto it = state_.ship_orders.find(ship_id);
    if (it == state_.ship_orders.end()) continue;
    auto& so = it->second;

    // Optional: auto-refill queue for simple repeating trade routes / patrol loops.
    // The queue is only refilled at the start of a tick so that the ship still
    // executes at most one order per day.
    if (so.queue.empty() && so.repeat && !so.repeat_template.empty()) {
      so.queue = so.repeat_template;
    }

    auto& q = so.queue;
    if (q.empty()) continue;

    // Wait order: consumes a simulation day without moving.
    if (std::holds_alternative<WaitDays>(q.front())) {
      auto& ord = std::get<WaitDays>(q.front());
      if (ord.days_remaining <= 0) {
        q.erase(q.begin());
        continue;
      }
      ord.days_remaining -= 1;
      if (ord.days_remaining <= 0) q.erase(q.begin());
      continue;
    }

    // Determine target.
    Vec2 target = ship.position_mkm;
    double desired_range = 0.0; // used for attack orders
    bool attack_has_contact = false;

    bool is_cargo = false;
    bool cargo_is_load = false;
    Id cargo_colony_id = kInvalidId;
    std::string cargo_mineral;
    double cargo_tons = 0.0;

    LoadMineral* load_ord = nullptr;
    UnloadMineral* unload_ord = nullptr;

    if (std::holds_alternative<MoveToPoint>(q.front())) {
      target = std::get<MoveToPoint>(q.front()).target_mkm;
    } else if (std::holds_alternative<MoveToBody>(q.front())) {
      const Id body_id = std::get<MoveToBody>(q.front()).body_id;
      const auto* body = find_ptr(state_.bodies, body_id);
      if (!body) {
        q.erase(q.begin());
        continue;
      }
      if (body->system_id != ship.system_id) {
        // Can't path across systems yet; drop order.
        q.erase(q.begin());
        continue;
      }
      target = body->position_mkm;
    } else if (std::holds_alternative<TravelViaJump>(q.front())) {
      const Id jump_id = std::get<TravelViaJump>(q.front()).jump_point_id;
      const auto* jp = find_ptr(state_.jump_points, jump_id);
      if (!jp || jp->system_id != ship.system_id) {
        q.erase(q.begin());
        continue;
      }
      target = jp->position_mkm;
    } else if (std::holds_alternative<AttackShip>(q.front())) {
      auto& ord = std::get<AttackShip>(q.front());
      const Id target_id = ord.target_ship_id;
      const auto* tgt = find_ptr(state_.ships, target_id);
      if (!tgt || tgt->system_id != ship.system_id) {
        q.erase(q.begin());
        continue;
      }

      // Only chase the true target position while we have contact; otherwise move to last-known.
      attack_has_contact = is_ship_detected_by_faction(ship.faction_id, target_id);

      if (attack_has_contact) {
        target = tgt->position_mkm;
        ord.last_known_position_mkm = target;
        ord.has_last_known = true;

        const auto* d = find_design(ship.design_id);
        const double w_range = d ? d->weapon_range_mkm : 0.0;
        desired_range = (w_range > 0.0) ? (w_range * 0.9) : 0.1;
      } else {
        if (!ord.has_last_known) {
          // Nothing to do; drop the order.
          q.erase(q.begin());
          continue;
        }
        target = ord.last_known_position_mkm;
        desired_range = 0.0;
      }
    } else if (std::holds_alternative<LoadMineral>(q.front())) {
      auto& ord = std::get<LoadMineral>(q.front());
      is_cargo = true;
      cargo_is_load = true;
      load_ord = &ord;
      cargo_colony_id = ord.colony_id;
      cargo_mineral = ord.mineral;
      cargo_tons = ord.tons;

      const auto* colony = find_ptr(state_.colonies, cargo_colony_id);
      if (!colony || colony->faction_id != ship.faction_id) {
        q.erase(q.begin());
        continue;
      }
      const auto* body = find_ptr(state_.bodies, colony->body_id);
      if (!body || body->system_id != ship.system_id) {
        q.erase(q.begin());
        continue;
      }
      target = body->position_mkm;
    } else if (std::holds_alternative<UnloadMineral>(q.front())) {
      auto& ord = std::get<UnloadMineral>(q.front());
      is_cargo = true;
      cargo_is_load = false;
      unload_ord = &ord;
      cargo_colony_id = ord.colony_id;
      cargo_mineral = ord.mineral;
      cargo_tons = ord.tons;

      const auto* colony = find_ptr(state_.colonies, cargo_colony_id);
      if (!colony || colony->faction_id != ship.faction_id) {
        q.erase(q.begin());
        continue;
      }
      const auto* body = find_ptr(state_.bodies, colony->body_id);
      if (!body || body->system_id != ship.system_id) {
        q.erase(q.begin());
        continue;
      }
      target = body->position_mkm;
    }

    const Vec2 delta = target - ship.position_mkm;
    const double dist = delta.length();

    const bool is_attack = std::holds_alternative<AttackShip>(q.front());
    const bool is_jump = std::holds_alternative<TravelViaJump>(q.front());
    const bool is_body = std::holds_alternative<MoveToBody>(q.front());

    const double arrive_eps = std::max(0.0, cfg_.arrival_epsilon_mkm);
    const double dock_range = std::max(arrive_eps, cfg_.docking_range_mkm);

    auto do_cargo_transfer = [&]() -> double {
      auto* colony = find_ptr(state_.colonies, cargo_colony_id);
      if (!colony) return 0.0;
      if (colony->faction_id != ship.faction_id) return 0.0;

      // Capacity only matters for load.
      const auto* d = find_design(ship.design_id);
      const double cap = d ? d->cargo_tons : 0.0;
      double used = cargo_used_tons(ship);
      double free = std::max(0.0, cap - used);

      const double kEps = 1e-9;

      double moved_total = 0.0;

      if (cargo_is_load) {
        // Nothing to load if no free capacity.
        if (free <= kEps) return 0.0;

        double remaining = (cargo_tons > 0.0) ? cargo_tons : 1e300;
        remaining = std::min(remaining, free);

        auto load_one = [&](const std::string& mineral, double max_tons) {
          if (max_tons <= kEps) return 0.0;
          auto it = colony->minerals.find(mineral);
          const double available = (it != colony->minerals.end()) ? std::max(0.0, it->second) : 0.0;
          const double take = std::min(available, max_tons);
          if (take > kEps) {
            if (it != colony->minerals.end()) {
              it->second = std::max(0.0, it->second - take);
            } else {
              colony->minerals[mineral] = 0.0;
            }
            ship.cargo[mineral] += take;
          }
          moved_total += take;
          return take;
        };

        if (!cargo_mineral.empty()) {
          load_one(cargo_mineral, remaining);
          return moved_total;
        }

        // Load from all minerals in a stable order.
        std::vector<std::string> keys;
        keys.reserve(colony->minerals.size());
        for (const auto& [k, v] : colony->minerals) {
          if (v > kEps) keys.push_back(k);
        }
        std::sort(keys.begin(), keys.end());

        for (const auto& k : keys) {
          if (remaining <= kEps) break;
          const double took = load_one(k, remaining);
          remaining -= took;
        }
        return moved_total;
      }

      // Unload
      double remaining = (cargo_tons > 0.0) ? cargo_tons : 1e300;

      auto unload_one = [&](const std::string& mineral, double max_tons) {
        if (max_tons <= kEps) return 0.0;
        auto it = ship.cargo.find(mineral);
        const double have = (it != ship.cargo.end()) ? std::max(0.0, it->second) : 0.0;
        const double put = std::min(have, max_tons);
        if (put > kEps) {
          colony->minerals[mineral] += put;
          if (it != ship.cargo.end()) {
            it->second = std::max(0.0, it->second - put);
            if (it->second <= kEps) ship.cargo.erase(it);
          }
          moved_total += put;
        }
        return put;
      };

      if (!cargo_mineral.empty()) {
        unload_one(cargo_mineral, remaining);
        return moved_total;
      }

      // Unload all cargo minerals in a stable order.
      std::vector<std::string> keys;
      keys.reserve(ship.cargo.size());
      for (const auto& [k, v] : ship.cargo) {
        if (v > kEps) keys.push_back(k);
      }
      std::sort(keys.begin(), keys.end());

      for (const auto& k : keys) {
        if (remaining <= kEps) break;
        const double put = unload_one(k, remaining);
        remaining -= put;
      }

      return moved_total;
    };

    auto cargo_order_complete = [&](double moved_this_tick) {
      // tons <= 0 => "as much as possible" in one go.
      if (cargo_tons <= 0.0) return true;

      // Reduce remaining tons.
      if (cargo_is_load && load_ord) {
        load_ord->tons = std::max(0.0, load_ord->tons - moved_this_tick);
        cargo_tons = load_ord->tons;
      }
      if (!cargo_is_load && unload_ord) {
        unload_ord->tons = std::max(0.0, unload_ord->tons - moved_this_tick);
        cargo_tons = unload_ord->tons;
      }

      if (cargo_tons <= 1e-9) return true;

      // If we couldn't move anything, decide whether to keep waiting.
      if (moved_this_tick <= 1e-9) {
        const auto* d = find_design(ship.design_id);
        const double cap = d ? d->cargo_tons : 0.0;
        const double free = std::max(0.0, cap - cargo_used_tons(ship));

        if (cargo_is_load) {
          // Ship is full; can't load more.
          if (free <= 1e-9) return true;
        } else {
          // Ship has nothing relevant to unload.
          if (!cargo_mineral.empty()) {
            auto it = ship.cargo.find(cargo_mineral);
            const double have = (it != ship.cargo.end()) ? it->second : 0.0;
            if (have <= 1e-9) return true;
          } else {
            if (ship.cargo.empty()) return true;
          }
        }
      }

      // Otherwise, keep the order at the front and try again next day.
      return false;
    };

    // Docked / arrived checks. Use a docking tolerance for moving body targets.
    if (is_cargo && dist <= dock_range) {
      ship.position_mkm = target;
      const double moved = do_cargo_transfer();
      if (cargo_order_complete(moved)) q.erase(q.begin());
      continue;
    }

    if (is_body && dist <= dock_range) {
      ship.position_mkm = target;
      q.erase(q.begin());
      continue;
    }

    if (!is_attack && !is_jump && !is_cargo && !is_body && dist <= arrive_eps) {
      q.erase(q.begin());
      continue;
    }

    auto transit_jump = [&]() {
      const Id jump_id = std::get<TravelViaJump>(q.front()).jump_point_id;
      const auto* jp = find_ptr(state_.jump_points, jump_id);
      if (!jp || jp->system_id != ship.system_id || jp->linked_jump_id == kInvalidId) return;

      const auto* dest = find_ptr(state_.jump_points, jp->linked_jump_id);
      if (!dest) return;

      const Id old_sys = ship.system_id;
      const Id new_sys = dest->system_id;

      // Remove ship from old system list.
      if (auto* sys_old = find_ptr(state_.systems, old_sys)) {
        sys_old->ships.erase(std::remove(sys_old->ships.begin(), sys_old->ships.end(), ship_id), sys_old->ships.end());
      }

      ship.system_id = new_sys;
      ship.position_mkm = dest->position_mkm;

      if (auto* sys_new = find_ptr(state_.systems, new_sys)) {
        sys_new->ships.push_back(ship_id);
      }

      // Exploration: entering a new system reveals it to the ship's faction.
      discover_system_for_faction(ship.faction_id, new_sys);

      {
        const auto* sys_new = find_ptr(state_.systems, new_sys);
        const std::string dest_name = sys_new ? sys_new->name : std::string("(unknown)");
        const std::string msg = "Ship " + ship.name + " transited jump point " + jp->name + " -> " + dest_name;
        nebula4x::log::info(msg);
        EventContext ctx;
        ctx.faction_id = ship.faction_id;
        ctx.system_id = new_sys;
        ctx.ship_id = ship_id;
        push_event(EventLevel::Info, EventCategory::Movement, msg, ctx);
      }
    };

    // Jump orders: within dock range should still transit (even if speed is 0).
    if (is_jump && dist <= dock_range) {
      ship.position_mkm = target;
      transit_jump();
      q.erase(q.begin());
      continue;
    }

    if (is_attack) {
      if (attack_has_contact) {
        // With contact: stop when within desired range.
        if (dist <= desired_range) {
          continue;
        }
      } else {
        // No contact: reaching last-known completes the order.
        if (dist <= arrive_eps) {
          q.erase(q.begin());
          continue;
        }
      }
    }

    const double max_step = mkm_per_day_from_speed(ship.speed_km_s, cfg_.seconds_per_day);
    if (max_step <= 0.0) continue;

    double step = max_step;
    if (is_attack) {
      step = std::min(step, std::max(0.0, dist - desired_range));
      if (step <= 0.0) continue;
    }

    if (dist <= step) {
      ship.position_mkm = target;

      if (is_jump) {
        transit_jump();
        q.erase(q.begin());
      } else if (is_attack) {
        // Attack orders persist while we have contact. If we were moving to last-known and reached it, complete.
        if (!attack_has_contact) q.erase(q.begin());
      } else if (is_cargo) {
        const double moved = do_cargo_transfer();
        if (cargo_order_complete(moved)) q.erase(q.begin());
      } else {
        q.erase(q.begin());
      }

      continue;
    }

    const Vec2 dir = delta.normalized();
    ship.position_mkm += dir * step;
  }
}


void Simulation::tick_combat() {
  // Simple day-based combat:
  // - Each armed ship fires once per day at either its explicit target (AttackShip order)
  //   or the closest hostile ship within range.
  // - Damage is applied simultaneously.

  std::unordered_map<Id, double> incoming_damage;
  std::unordered_map<Id, std::vector<Id>> attackers_for_target;

  auto is_hostile = [&](const Ship& a, const Ship& b) { return a.faction_id != b.faction_id; };

  // Deterministic: iterate attackers/targets in a stable id order.
  // This avoids cross-platform nondeterminism from unordered_map iteration.
  const auto ship_ids = sorted_keys(state_.ships);

  for (Id aid : ship_ids) {
    const auto* attacker_ptr = find_ptr(state_.ships, aid);
    if (!attacker_ptr) continue;
    const auto& attacker = *attacker_ptr;
    const auto* ad = find_design(attacker.design_id);
    if (!ad) continue;
    if (ad->weapon_damage <= 0.0 || ad->weapon_range_mkm <= 0.0) continue;

    // Candidate targets: ships in same system and hostile.
    Id chosen = kInvalidId;
    double chosen_dist = 1e300;

    // If explicit target exists (front order), prefer it.
    auto oit = state_.ship_orders.find(aid);
    if (oit != state_.ship_orders.end() && !oit->second.queue.empty() &&
        std::holds_alternative<AttackShip>(oit->second.queue.front())) {
      const Id tid = std::get<AttackShip>(oit->second.queue.front()).target_ship_id;
      const auto* tgt = find_ptr(state_.ships, tid);
      if (tgt && tgt->system_id == attacker.system_id && is_hostile(attacker, *tgt) &&
          is_ship_detected_by_faction(attacker.faction_id, tid)) {
        const double dist = (tgt->position_mkm - attacker.position_mkm).length();
        if (dist <= ad->weapon_range_mkm) {
          chosen = tid;
          chosen_dist = dist;
        }
      }
    }

    // Otherwise, pick closest hostile within range.
    if (chosen == kInvalidId) {
      for (Id bid : ship_ids) {
        if (bid == aid) continue;
        const auto* target_ptr = find_ptr(state_.ships, bid);
        if (!target_ptr) continue;
        const auto& target = *target_ptr;

        if (target.system_id != attacker.system_id) continue;
        if (!is_hostile(attacker, target)) continue;
        if (!is_ship_detected_by_faction(attacker.faction_id, bid)) continue;

        const double dist = (target.position_mkm - attacker.position_mkm).length();
        if (dist > ad->weapon_range_mkm) continue;
        if (dist < chosen_dist) {
          chosen = bid;
          chosen_dist = dist;
        }
      }
    }

    if (chosen != kInvalidId) {
      incoming_damage[chosen] += ad->weapon_damage;
      attackers_for_target[chosen].push_back(aid);
    }
  }

  if (incoming_damage.empty()) return;

  std::vector<Id> destroyed;
  destroyed.reserve(incoming_damage.size());

  for (Id tid : sorted_keys(incoming_damage)) {
    const double dmg = incoming_damage[tid];
    auto* tgt = find_ptr(state_.ships, tid);
    if (!tgt) continue;
    tgt->hp -= dmg;
    if (tgt->hp <= 0.0) destroyed.push_back(tid);
  }

  // Deterministic: stable log + removal ordering.
  std::sort(destroyed.begin(), destroyed.end());

  // Pre-compute destruction messages before mutating the ship map so we can
  // still reference attacker ships even if they are also destroyed this tick.
  struct DestructionEvent {
    std::string msg;
    EventContext ctx;
  };
  std::vector<DestructionEvent> death_events;
  death_events.reserve(destroyed.size());

  for (Id dead_id : destroyed) {
    const auto it = state_.ships.find(dead_id);
    if (it == state_.ships.end()) continue;

    const Ship& victim = it->second;
    const Id sys_id = victim.system_id;
    const Id victim_fid = victim.faction_id;

    const auto* sys = find_ptr(state_.systems, sys_id);
    const std::string sys_name = sys ? sys->name : std::string("(unknown)");

    const auto* victim_fac = find_ptr(state_.factions, victim_fid);
    const std::string victim_fac_name = victim_fac ? victim_fac->name : std::string("(unknown)");

    Id attacker_ship_id = kInvalidId;
    Id attacker_fid = kInvalidId;
    std::string attacker_ship_name;
    std::string attacker_fac_name;
    std::size_t attackers_count = 0;

    if (auto ita = attackers_for_target.find(dead_id); ita != attackers_for_target.end()) {
      auto& vec = ita->second;
      std::sort(vec.begin(), vec.end());
      vec.erase(std::unique(vec.begin(), vec.end()), vec.end());
      attackers_count = vec.size();
      if (!vec.empty()) {
        attacker_ship_id = vec.front();
        if (const auto* atk = find_ptr(state_.ships, attacker_ship_id)) {
          attacker_fid = atk->faction_id;
          attacker_ship_name = atk->name;
          if (const auto* af = find_ptr(state_.factions, attacker_fid)) attacker_fac_name = af->name;
        }
      }
    }

    EventContext ctx;
    ctx.faction_id = victim_fid;
    ctx.faction_id2 = attacker_fid;
    ctx.system_id = sys_id;
    ctx.ship_id = dead_id;

    std::string msg = "Ship destroyed: " + victim.name;
    msg += " (" + victim_fac_name + ")";
    msg += " in " + sys_name;

    if (attacker_ship_id != kInvalidId) {
      msg += " (killed by " + (attacker_ship_name.empty() ? std::string("Ship ") + std::to_string(attacker_ship_id)
                                                         : attacker_ship_name);
      if (!attacker_fac_name.empty()) msg += " / " + attacker_fac_name;
      if (attackers_count > 1) msg += " +" + std::to_string(attackers_count - 1) + " more";
      msg += ")";
    }

    death_events.push_back(DestructionEvent{std::move(msg), ctx});
  }

  for (Id dead_id : destroyed) {
    auto it = state_.ships.find(dead_id);
    if (it == state_.ships.end()) continue;

    const Id sys_id = it->second.system_id;

    // Remove from system.
    if (auto* sys = find_ptr(state_.systems, sys_id)) {
      sys->ships.erase(std::remove(sys->ships.begin(), sys->ships.end(), dead_id), sys->ships.end());
    }

    state_.ship_orders.erase(dead_id);
    state_.ships.erase(dead_id);

    // Clear any remembered contacts for this ship.
    for (auto& [_, fac] : state_.factions) {
      fac.ship_contacts.erase(dead_id);
    }

  }

  // Log destruction events after state mutation.
  for (const auto& e : death_events) {
    nebula4x::log::warn(e.msg);
    push_event(EventLevel::Warn, EventCategory::Combat, e.msg, e.ctx);
  }
}

} // namespace nebula4x
