#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>
#include <unordered_map>

#include "nebula4x/core/simulation.h"

#define N4X_ASSERT(expr)                                                                            \
  do {                                                                                              \
    if (!(expr)) {                                                                                  \
      std::cerr << "ASSERT failed: " #expr " (" << __FILE__ << ":" << __LINE__ << ")\n";           \
      return 1;                                                                                     \
    }                                                                                               \
  } while (0)

namespace {

double cargo_tons(const nebula4x::Ship& s, const std::string& mineral) {
  auto it = s.cargo.find(mineral);
  return (it != s.cargo.end()) ? it->second : 0.0;
}

nebula4x::GameState make_cargo_competition_state(bool reverse_ship_insertion) {
  using namespace nebula4x;

  GameState s;
  s.save_version = 10;
  s.date = Date::from_ymd(2200, 1, 1);
  s.next_id = 1;

  const Id fac_id = allocate_id(s);
  {
    Faction f;
    f.id = fac_id;
    f.name = "Faction";
    s.factions[fac_id] = f;
  }

  const Id sys_id = allocate_id(s);
  {
    StarSystem sys;
    sys.id = sys_id;
    sys.name = "Sol";
    sys.galaxy_pos = {0.0, 0.0};
    s.systems[sys_id] = sys;
  }

  // Pre-seed discovery so load_game() doesn't introduce ordering differences
  // via unordered_map iteration.
  s.factions[fac_id].discovered_systems = {sys_id};

  const Id earth_id = allocate_id(s);
  {
    Body b;
    b.id = earth_id;
    b.name = "Earth";
    b.type = BodyType::Planet;
    b.system_id = sys_id;
    b.orbit_radius_mkm = 149.6;
    b.orbit_period_days = 365.25;
    b.orbit_phase_radians = 0.0;
    s.bodies[earth_id] = b;
    s.systems[sys_id].bodies.push_back(earth_id);
  }

  const Id colony_id = allocate_id(s);
  {
    Colony c;
    c.id = colony_id;
    c.name = "Earth";
    c.faction_id = fac_id;
    c.body_id = earth_id;
    c.population_millions = 1000.0;
    c.minerals = {{"Duranium", 50.0}};
    s.colonies[colony_id] = c;
  }

  const Id ship_a_id = allocate_id(s);
  Ship ship_a;
  ship_a.id = ship_a_id;
  ship_a.name = "Ship A";
  ship_a.faction_id = fac_id;
  ship_a.system_id = sys_id;
  ship_a.position_mkm = {149.6, 0.0};
  ship_a.design_id = "cargo";

  const Id ship_b_id = allocate_id(s);
  Ship ship_b;
  ship_b.id = ship_b_id;
  ship_b.name = "Ship B";
  ship_b.faction_id = fac_id;
  ship_b.system_id = sys_id;
  ship_b.position_mkm = {149.6, 0.0};
  ship_b.design_id = "cargo";

  // Force all keys into a single bucket so unordered_map iteration order is
  // strongly influenced by insertion order. This makes nondeterminism easy to
  // reproduce in tests.
  std::unordered_map<Id, Ship> ships;
  ships.max_load_factor(1000.0f);
  ships.rehash(1);
  if (reverse_ship_insertion) {
    ships.emplace(ship_b_id, ship_b);
    ships.emplace(ship_a_id, ship_a);
  } else {
    ships.emplace(ship_a_id, ship_a);
    ships.emplace(ship_b_id, ship_b);
  }
  s.ships = std::move(ships);

  // System ship list is only used for sensors; keep it consistent for both states.
  s.systems[sys_id].ships = {ship_a_id, ship_b_id};

  // Both ships attempt to load the full stockpile. Whichever ship ticks first
  // will get the minerals.
  {
    ShipOrders oa;
    oa.queue.push_back(LoadMineral{colony_id, "Duranium", 50.0});
    s.ship_orders[ship_a_id] = oa;
  }
  {
    ShipOrders ob;
    ob.queue.push_back(LoadMineral{colony_id, "Duranium", 50.0});
    s.ship_orders[ship_b_id] = ob;
  }

  s.selected_system = sys_id;
  return s;
}

nebula4x::GameState make_combat_tiebreak_state(bool reverse_target_insertion) {
  using namespace nebula4x;

  GameState s;
  s.save_version = 10;
  s.date = Date::from_ymd(2200, 1, 1);
  s.next_id = 1;

  const Id fac_a = allocate_id(s);
  {
    Faction f;
    f.id = fac_a;
    f.name = "Attacker";
    s.factions[fac_a] = f;
  }
  const Id fac_b = allocate_id(s);
  {
    Faction f;
    f.id = fac_b;
    f.name = "Target";
    s.factions[fac_b] = f;
  }

  const Id sys_id = allocate_id(s);
  {
    StarSystem sys;
    sys.id = sys_id;
    sys.name = "Arena";
    sys.galaxy_pos = {0.0, 0.0};
    s.systems[sys_id] = sys;
  }

  // Pre-seed discovery to avoid ordering differences on load.
  s.factions[fac_a].discovered_systems = {sys_id};
  s.factions[fac_b].discovered_systems = {sys_id};

  const Id attacker_id = allocate_id(s);
  Ship attacker;
  attacker.id = attacker_id;
  attacker.name = "Attacker";
  attacker.faction_id = fac_a;
  attacker.system_id = sys_id;
  attacker.position_mkm = {0.0, 0.0};
  attacker.design_id = "attacker";

  const Id t1_id = allocate_id(s);
  Ship t1;
  t1.id = t1_id;
  t1.name = "Target 1";
  t1.faction_id = fac_b;
  t1.system_id = sys_id;
  t1.position_mkm = {10.0, 0.0};
  t1.design_id = "target";

  const Id t2_id = allocate_id(s);
  Ship t2;
  t2.id = t2_id;
  t2.name = "Target 2";
  t2.faction_id = fac_b;
  t2.system_id = sys_id;
  t2.position_mkm = {-10.0, 0.0};
  t2.design_id = "target";

  std::unordered_map<Id, Ship> ships;
  ships.max_load_factor(1000.0f);
  ships.rehash(1);

  // Insert attacker first (same for both), then vary insertion order of tied targets.
  ships.emplace(attacker_id, attacker);
  if (reverse_target_insertion) {
    ships.emplace(t2_id, t2);
    ships.emplace(t1_id, t1);
  } else {
    ships.emplace(t1_id, t1);
    ships.emplace(t2_id, t2);
  }

  s.ships = std::move(ships);
  s.systems[sys_id].ships = {attacker_id, t1_id, t2_id};
  s.selected_system = sys_id;
  return s;
}

} // namespace

int test_determinism() {
  // --- deterministic ship-tick ordering (cargo competition) ---
  {
    nebula4x::ContentDB content;

    nebula4x::ShipDesign cargo;
    cargo.id = "cargo";
    cargo.name = "Cargo";
    cargo.max_hp = 10.0;
    cargo.speed_km_s = 0.0;
    cargo.cargo_tons = 1000.0;
    content.designs[cargo.id] = cargo;

    nebula4x::Simulation sim_a(content, nebula4x::SimConfig{});
    nebula4x::Simulation sim_b(content, nebula4x::SimConfig{});

    auto st_a = make_cargo_competition_state(false);
    auto st_b = make_cargo_competition_state(true);

    // Sanity: the same ship ids exist in both...
    N4X_ASSERT(st_a.ships.size() == 2);
    N4X_ASSERT(st_b.ships.size() == 2);

    // Grab ids so we can query ships after ticking.
    std::vector<nebula4x::Id> ship_ids;
    for (const auto& [id, _] : st_a.ships) ship_ids.push_back(id);
    std::sort(ship_ids.begin(), ship_ids.end());
    N4X_ASSERT(ship_ids.size() == 2);
    const auto ship_low = ship_ids[0];
    const auto ship_high = ship_ids[1];

    sim_a.load_game(std::move(st_a));
    sim_b.load_game(std::move(st_b));

    sim_a.advance_days(1);
    sim_b.advance_days(1);

    const auto* a_low = nebula4x::find_ptr(sim_a.state().ships, ship_low);
    const auto* a_high = nebula4x::find_ptr(sim_a.state().ships, ship_high);
    const auto* b_low = nebula4x::find_ptr(sim_b.state().ships, ship_low);
    const auto* b_high = nebula4x::find_ptr(sim_b.state().ships, ship_high);
    N4X_ASSERT(a_low && a_high && b_low && b_high);

    // Both simulations should produce the same outcome regardless of unordered_map insertion order.
    N4X_ASSERT(std::fabs(cargo_tons(*a_low, "Duranium") - cargo_tons(*b_low, "Duranium")) < 1e-9);
    N4X_ASSERT(std::fabs(cargo_tons(*a_high, "Duranium") - cargo_tons(*b_high, "Duranium")) < 1e-9);

    // Defined behavior: lower id ship gets the limited stockpile first.
    N4X_ASSERT(std::fabs(cargo_tons(*a_low, "Duranium") - 50.0) < 1e-9);
    N4X_ASSERT(std::fabs(cargo_tons(*a_high, "Duranium") - 0.0) < 1e-9);
  }

  // --- deterministic combat target selection (tie break) ---
  {
    nebula4x::ContentDB content;

    nebula4x::ShipDesign attacker;
    attacker.id = "attacker";
    attacker.name = "Attacker";
    attacker.max_hp = 100.0;
    attacker.speed_km_s = 0.0;
    attacker.sensor_range_mkm = 100.0;
    attacker.weapon_damage = 10.0;
    attacker.weapon_range_mkm = 20.0;
    content.designs[attacker.id] = attacker;

    nebula4x::ShipDesign target;
    target.id = "target";
    target.name = "Target";
    target.max_hp = 100.0;
    target.speed_km_s = 0.0;
    target.sensor_range_mkm = 0.0;
    target.weapon_damage = 0.0;
    target.weapon_range_mkm = 0.0;
    content.designs[target.id] = target;

    nebula4x::Simulation sim_a(content, nebula4x::SimConfig{});
    nebula4x::Simulation sim_b(content, nebula4x::SimConfig{});

    auto st_a = make_combat_tiebreak_state(false);
    auto st_b = make_combat_tiebreak_state(true);

    // Identify target ids.
    std::vector<nebula4x::Id> targets;
    for (const auto& [id, sh] : st_a.ships) {
      if (sh.design_id == "target") targets.push_back(id);
    }
    // Sanity: exactly two targets are present.
    N4X_ASSERT(targets.size() == 2);
    std::sort(targets.begin(), targets.end());
    const auto target_low = targets[0];
    const auto target_high = targets[1];

    sim_a.load_game(std::move(st_a));
    sim_b.load_game(std::move(st_b));

    sim_a.advance_days(1);
    sim_b.advance_days(1);

    const auto* a_tl = nebula4x::find_ptr(sim_a.state().ships, target_low);
    const auto* a_th = nebula4x::find_ptr(sim_a.state().ships, target_high);
    const auto* b_tl = nebula4x::find_ptr(sim_b.state().ships, target_low);
    const auto* b_th = nebula4x::find_ptr(sim_b.state().ships, target_high);
    N4X_ASSERT(a_tl && a_th && b_tl && b_th);

    // Both simulations should agree even when unordered_map insertion order differs.
    N4X_ASSERT(std::fabs(a_tl->hp - b_tl->hp) < 1e-9);
    N4X_ASSERT(std::fabs(a_th->hp - b_th->hp) < 1e-9);

    // Defined tie-break: lower id target is selected when distances are equal.
    N4X_ASSERT(std::fabs(a_tl->hp - 90.0) < 1e-9);
    N4X_ASSERT(std::fabs(a_th->hp - 100.0) < 1e-9);
  }

  return 0;
}
