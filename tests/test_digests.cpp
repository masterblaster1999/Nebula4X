#include <iostream>
#include <string>
#include <vector>

#include "nebula4x/core/date.h"
#include "nebula4x/util/digest.h"
#include "nebula4x/util/json.h"
#include "nebula4x/util/timeline_export.h"

#define N4X_ASSERT(expr) \
  do { \
    if (!(expr)) { \
      std::cerr << "ASSERT failed: " #expr " (" << __FILE__ << ":" << __LINE__ << ")\n"; \
      return 1; \
    } \
  } while (0)

int test_digests() {
  using namespace nebula4x;

  // --- Content digest is stable across unordered_map insertion order ---
  ContentDB c1;
  {
    ComponentDef engine;
    engine.id = "engine_basic";
    engine.name = "Basic Engine";
    engine.type = ComponentType::Engine;
    engine.mass_tons = 10.0;
    engine.speed_km_s = 5000.0;
    engine.fuel_use_per_mkm = 0.5;
    c1.components[engine.id] = engine;

    InstallationDef mine;
    mine.id = "mine";
    mine.name = "Mine";
    mine.mining = true;
    mine.produces_per_day["Duranium"] = 1.0;
    mine.build_costs["Duranium"] = 5.0;
    c1.installations[mine.id] = mine;

    ShipDesign scout;
    scout.id = "scout";
    scout.name = "Scout";
    scout.role = ShipRole::Surveyor;
    scout.components = {"engine_basic"};
    scout.mass_tons = 10.0;
    scout.speed_km_s = 5000.0;
    scout.fuel_capacity_tons = 50.0;
    scout.fuel_use_per_mkm = 0.5;
    c1.designs[scout.id] = scout;

    TechDef tech;
    tech.id = "T1";
    tech.name = "Test Tech";
    tech.cost = 100.0;
    tech.prereqs = {"T0"};
    tech.effects.push_back(TechEffect{"unlock_component", "engine_basic", 0.0});
    c1.techs[tech.id] = tech;
  }

  ContentDB c2;
  {
    // Insert in a different order.
    TechDef tech;
    tech.id = "T1";
    tech.name = "Test Tech";
    tech.cost = 100.0;
    tech.prereqs = {"T0"};
    tech.effects.push_back(TechEffect{"unlock_component", "engine_basic", 0.0});
    c2.techs[tech.id] = tech;

    ShipDesign scout;
    scout.id = "scout";
    scout.name = "Scout";
    scout.role = ShipRole::Surveyor;
    scout.components = {"engine_basic"};
    scout.mass_tons = 10.0;
    scout.speed_km_s = 5000.0;
    scout.fuel_capacity_tons = 50.0;
    scout.fuel_use_per_mkm = 0.5;
    c2.designs[scout.id] = scout;

    InstallationDef mine;
    mine.id = "mine";
    mine.name = "Mine";
    mine.mining = true;
    mine.build_costs["Duranium"] = 5.0;
    mine.produces_per_day["Duranium"] = 1.0;
    c2.installations[mine.id] = mine;

    ComponentDef engine;
    engine.id = "engine_basic";
    engine.name = "Basic Engine";
    engine.type = ComponentType::Engine;
    engine.mass_tons = 10.0;
    engine.speed_km_s = 5000.0;
    engine.fuel_use_per_mkm = 0.5;
    c2.components[engine.id] = engine;
  }

  const auto cd1 = digest_content_db64(c1);
  const auto cd2 = digest_content_db64(c2);
  N4X_ASSERT(cd1 == cd2);

  // --- GameState digest is stable across unordered_map insertion order ---
  GameState s1;
  s1.save_version = 26;
  s1.date = Date::from_ymd(2200, 1, 1);
  s1.next_id = 123;
  s1.next_event_seq = 42;
  s1.selected_system = 1;

  // Systems
  {
    StarSystem sys;
    sys.name = "Sol";
    sys.galaxy_pos = Vec2{0.0, 0.0};
    sys.bodies = {11, 10};
    sys.ships = {101, 100};
    sys.jump_points = {200};
    s1.systems[1] = sys;
  }

  // Bodies
  {
    Body earth;
    earth.name = "Earth";
    earth.type = BodyType::Planet;
    earth.system_id = 1;
    earth.orbit_radius_mkm = 150000.0;
    earth.orbit_period_days = 365.0;
    earth.orbit_phase_radians = 1.0;
    earth.position_mkm = Vec2{150000.0, 0.0};
    earth.mineral_deposits["Neutronium"] = 5.0;
    earth.mineral_deposits["Duranium"] = 10.0;
    s1.bodies[10] = earth;

    Body luna;
    luna.name = "Luna";
    luna.type = BodyType::Moon;
    luna.system_id = 1;
    luna.parent_body_id = 10;
    luna.orbit_radius_mkm = 0.384;
    luna.orbit_period_days = 27.0;
    luna.orbit_phase_radians = 0.5;
    luna.position_mkm = Vec2{150000.384, 0.0};
    s1.bodies[11] = luna;
  }

  // Jump point
  {
    JumpPoint jp;
    jp.name = "Sol-J1";
    jp.system_id = 1;
    jp.position_mkm = Vec2{1000.0, 1000.0};
    jp.linked_jump_id = kInvalidId;
    s1.jump_points[200] = jp;
  }

  // Factions
  {
    Faction f;
    f.name = "Humans";
    f.control = FactionControl::Player;
    f.research_points = 12.5;
    f.active_research_id = "T1";
    f.active_research_progress = 1.25;
    f.research_queue = {"T2", "T3"};
    f.known_techs = {"T0", "T1"};
    f.unlocked_components = {"engine_basic"};
    f.discovered_systems = {1};
    s1.factions[1] = f;
  }

  // Ships
  {
    Ship a;
    a.name = "Scout A";
    a.faction_id = 1;
    a.system_id = 1;
    a.position_mkm = Vec2{0.0, 0.0};
    a.design_id = "scout";
    a.speed_km_s = 5000.0;
    a.cargo["Duranium"] = 2.0;
    a.cargo["Neutronium"] = 1.0;
    a.auto_explore = true;
    s1.ships[100] = a;

    Ship b;
    b.name = "Scout B";
    b.faction_id = 1;
    b.system_id = 1;
    b.position_mkm = Vec2{10.0, 0.0};
    b.design_id = "scout";
    b.speed_km_s = 5000.0;
    b.cargo["Neutronium"] = 3.0;
    b.cargo["Duranium"] = 4.0;
    s1.ships[101] = b;
  }

  // Colony
  {
    Colony col;
    col.name = "Earth";
    col.faction_id = 1;
    col.body_id = 10;
    col.population_millions = 100.0;
    col.minerals["Duranium"] = 1000.0;
    col.minerals["Neutronium"] = 500.0;
    s1.colonies[500] = col;
  }

  // Fleet
  {
    Fleet fl;
    fl.name = "1st Fleet";
    fl.faction_id = 1;
    fl.leader_ship_id = 100;
    fl.ship_ids = {101, 100};
    fl.formation = FleetFormation::LineAbreast;
    fl.formation_spacing_mkm = 5.0;
    s1.fleets[700] = fl;
  }

  // Orders
  {
    ShipOrders so;
    so.queue.push_back(MoveToBody{10});
    so.queue.push_back(OrbitBody{10, 5});
    so.repeat = false;
    s1.ship_orders[100] = so;
  }

  // One event
  {
    SimEvent e;
    e.seq = 41;
    e.day = 0;
    e.level = EventLevel::Info;
    e.category = EventCategory::General;
    e.faction_id = 1;
    e.message = "hello";
    s1.events.push_back(e);
  }

  // Same data but inserted in different map/key order and with different set ordering.
  GameState s2;
  s2.save_version = s1.save_version;
  s2.date = s1.date;
  s2.next_id = s1.next_id;
  s2.next_event_seq = s1.next_event_seq;
  s2.selected_system = s1.selected_system;

  // Insert ships reversed.
  s2.ships[101] = s1.ships.at(101);
  s2.ships[100] = s1.ships.at(100);

  // Insert bodies reversed.
  s2.bodies[11] = s1.bodies.at(11);
  s2.bodies[10] = s1.bodies.at(10);

  // Jump points.
  s2.jump_points[200] = s1.jump_points.at(200);

  // System with different vector ordering.
  {
    StarSystem sys = s1.systems.at(1);
    sys.bodies = {10, 11};
    sys.ships = {100, 101};
    sys.jump_points = {200};
    s2.systems[1] = sys;
  }

  // Faction inserted after other maps.
  {
    Faction f = s1.factions.at(1);
    f.known_techs = {"T1", "T0"};
    f.discovered_systems = {1};
    s2.factions[1] = f;
  }

  // Colony inserted.
  s2.colonies[500] = s1.colonies.at(500);

  // Fleet with different ship id ordering.
  {
    Fleet fl = s1.fleets.at(700);
    fl.ship_ids = {100, 101};
    s2.fleets[700] = fl;
  }

  // Ship orders (order-sensitive) kept identical.
  s2.ship_orders[100] = s1.ship_orders.at(100);

  // Events identical.
  s2.events = s1.events;

  const auto gd1 = digest_game_state64(s1);
  const auto gd2 = digest_game_state64(s2);
  N4X_ASSERT(gd1 == gd2);

  // Digest options: UI state can be excluded.
  GameState s3 = s1;
  s3.selected_system = 999;
  DigestOptions no_ui;
  no_ui.include_events = true;
  no_ui.include_ui_state = false;
  N4X_ASSERT(digest_game_state64(s1, no_ui) == digest_game_state64(s3, no_ui));

  // Digest options: event log can be excluded.
  GameState s4 = s1;
  s4.events[0].message = "different";
  DigestOptions no_events;
  no_events.include_events = false;
  no_events.include_ui_state = true;
  N4X_ASSERT(digest_game_state64(s1, no_events) == digest_game_state64(s4, no_events));

  // Digest breakdown report: overall matches digest_game_state64(), and parts isolate changes.
  {
    const auto rep = digest_game_state64_report(s1);
    N4X_ASSERT(rep.overall == digest_game_state64(s1));

    bool found_ship_orders = false;
    std::uint64_t ship_orders_digest = 0;
    for (const auto& p : rep.parts) {
      if (p.label == "ship_orders") {
        found_ship_orders = true;
        ship_orders_digest = p.digest;
      }
    }
    N4X_ASSERT(found_ship_orders);

    // Change only ship orders, and ensure the ship_orders part digest changes.
    GameState s5 = s1;
    s5.ship_orders[100].queue.push_back(WaitDays{1});
    const auto rep2 = digest_game_state64_report(s5);

    std::uint64_t ship_orders_digest2 = 0;
    for (const auto& p : rep2.parts) {
      if (p.label == "ship_orders") ship_orders_digest2 = p.digest;
    }
    N4X_ASSERT(ship_orders_digest != ship_orders_digest2);

    // Coverage: mutating fields inside complex orders should affect the digest.
    auto ship_orders_part_digest = [](const GameState& s) -> std::uint64_t {
      const auto r = digest_game_state64_report(s);
      for (const auto& p : r.parts) {
        if (p.label == "ship_orders") return p.digest;
      }
      return 0;
    };

    // AttackShip: last_known_day (and other tracking fields) are part of persisted state.
    {
      GameState a = s1;
      ShipOrders so;
      AttackShip atk;
      atk.target_ship_id = 101;
      atk.has_last_known = true;
      atk.last_known_position_mkm = Vec2{123.0, 456.0};
      atk.last_known_system_id = 1;
      atk.last_known_day = 5;
      atk.pursuit_hops = 2;
      atk.search_waypoint_index = 3;
      atk.has_search_offset = true;
      atk.search_offset_mkm = Vec2{7.0, 8.0};
      so.queue.push_back(atk);
      a.ship_orders[100] = so;

      const auto d1 = ship_orders_part_digest(a);
      GameState b = a;
      std::get<AttackShip>(b.ship_orders[100].queue[0]).last_known_day += 1;
      const auto d2 = ship_orders_part_digest(b);
      N4X_ASSERT(d1 != d2);
    }

    // EscortShip: allow_neutral affects validity/behavior (contracts/escorts).
    {
      GameState a = s1;
      ShipOrders so;
      EscortShip esc;
      esc.target_ship_id = 101;
      esc.follow_distance_mkm = 1.25;
      esc.restrict_to_discovered = false;
      esc.allow_neutral = false;
      so.queue.push_back(esc);
      a.ship_orders[100] = so;

      const auto d1 = ship_orders_part_digest(a);
      GameState b = a;
      std::get<EscortShip>(b.ship_orders[100].queue[0]).allow_neutral = true;
      const auto d2 = ship_orders_part_digest(b);
      N4X_ASSERT(d1 != d2);
    }

    // BombardColony: progress_days is part of the order and must influence the digest.
    {
      GameState a = s1;
      ShipOrders so;
      BombardColony bmb;
      bmb.colony_id = 500;
      bmb.duration_days = 2;
      bmb.progress_days = 0.25;
      so.queue.push_back(bmb);
      a.ship_orders[100] = so;

      const auto d1 = ship_orders_part_digest(a);
      GameState b = a;
      std::get<BombardColony>(b.ship_orders[100].queue[0]).progress_days = 0.75;
      const auto d2 = ship_orders_part_digest(b);
      N4X_ASSERT(d1 != d2);
    }
  }

  // --- Timeline export smoke test ---
  TimelineExportOptions topt;
  topt.include_minerals = true;
  topt.include_ship_cargo = true;
  topt.mineral_filter = {"Duranium"};
  topt.digest = no_events;  // ignore events for this snapshot.

  std::vector<TimelineSnapshot> snaps;
  snaps.push_back(compute_timeline_snapshot(s1, c1, cd1, s1.next_event_seq, topt));
  const std::string jsonl = timeline_snapshots_to_jsonl(snaps);

  // One line + trailing newline.
  const auto nl = jsonl.find('\n');
  N4X_ASSERT(nl != std::string::npos);
  const std::string line = jsonl.substr(0, nl);
  const auto v = json::parse(line);
  N4X_ASSERT(v.is_object());
  const auto& o = v.object();
  N4X_ASSERT(o.find("day") != o.end());
  N4X_ASSERT(o.find("state_digest") != o.end());
  N4X_ASSERT(o.find("content_digest") != o.end());
  N4X_ASSERT(o.find("factions") != o.end());

  // Ensure mineral filter is applied.
  const auto& factions = o.at("factions").array();
  N4X_ASSERT(!factions.empty());
  const auto& f0 = factions[0].object();
  N4X_ASSERT(f0.find("minerals") != f0.end());
  const auto& mins = f0.at("minerals").object();
  N4X_ASSERT(mins.find("Duranium") != mins.end());
  N4X_ASSERT(mins.find("Neutronium") == mins.end());

  return 0;
}
