#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

#include "nebula4x/core/serialization.h"
#include "nebula4x/core/simulation.h"

#define N4X_ASSERT(expr)                                                                            \
  do {                                                                                              \
    if (!(expr)) {                                                                                  \
      std::cerr << "ASSERT failed: " #expr " (" << __FILE__ << ":" << __LINE__ << ")\n";        \
      return 1;                                                                                     \
    }                                                                                               \
  } while (0)

namespace {

nebula4x::GameState make_minimal_state() {
  using namespace nebula4x;

  GameState s;
  s.save_version = GameState{}.save_version;
  s.date = Date::from_ymd(2200, 1, 1);
  s.next_id = 100;

  // Two factions.
  {
    Faction f;
    f.id = 1;
    f.name = "Terrans";
    s.factions[f.id] = f;
  }
  {
    Faction f;
    f.id = 2;
    f.name = "Martians";
    s.factions[f.id] = f;
  }

  // Minimal star system.
  {
    StarSystem sys;
    sys.id = 10;
    sys.name = "Sol";
    s.systems[sys.id] = sys;
  }

  // Three ships.
  {
    Ship a;
    a.id = 10;
    a.name = "A";
    a.faction_id = 1;
    a.system_id = 10;
    a.design_id = "";
    s.ships[a.id] = a;
    s.systems[a.system_id].ships.push_back(a.id);
  }
  {
    Ship b;
    b.id = 11;
    b.name = "B";
    b.faction_id = 1;
    b.system_id = 10;
    b.design_id = "";
    s.ships[b.id] = b;
    s.systems[b.system_id].ships.push_back(b.id);
  }
  {
    Ship c;
    c.id = 12;
    c.name = "C";
    c.faction_id = 2;
    c.system_id = 10;
    c.design_id = "";
    s.ships[c.id] = c;
    s.systems[c.system_id].ships.push_back(c.id);
  }

  s.selected_system = 10;
  return s;
}

nebula4x::GameState make_defend_mission_state() {
  using namespace nebula4x;

  GameState s = make_minimal_state();

  // Add a single body + colony to defend.
  {
    Body b;
    b.id = 100;
    b.name = "Earth";
    b.system_id = 10;
    b.position_mkm = Vec2{0.0, 0.0};
    s.bodies[b.id] = b;
    s.systems[b.system_id].bodies.push_back(b.id);

    Colony c;
    c.id = 500;
    c.name = "Earth Colony";
    c.faction_id = 1;
    c.body_id = b.id;
    s.colonies[c.id] = c;
  }

  // Place ships within sensor range.
  s.ships.at(10).position_mkm = Vec2{0.0, 0.0};
  s.ships.at(11).position_mkm = Vec2{0.0, 1.0};
  s.ships.at(12).position_mkm = Vec2{100.0, 0.0};

  return s;
}

nebula4x::GameState make_escort_mission_state() {
  using namespace nebula4x;

  GameState s = make_minimal_state();

  // Add a single freighter to escort.
  {
    Ship f;
    f.id = 13;
    f.name = "Freighter";
    f.faction_id = 1;
    f.system_id = 10;
    f.design_id = "freighter";
    f.auto_freight = true;
    f.position_mkm = Vec2{0.0, 0.0};
    s.ships[f.id] = f;
    s.systems[f.system_id].ships.push_back(f.id);
  }

  // Position the escorts near the freighter and keep the hostile far away.
  s.ships.at(10).position_mkm = Vec2{-1.0, 0.0};
  s.ships.at(11).position_mkm = Vec2{-1.0, 1.0};
  s.ships.at(12).position_mkm = Vec2{100.0, 0.0};

  return s;
}

nebula4x::GameState make_jump_state() {
  using namespace nebula4x;

  GameState s;
  s.save_version = GameState{}.save_version;
  s.date = Date::from_ymd(2200, 1, 1);
  s.next_id = 2000;

  // One faction.
  {
    Faction f;
    f.id = 1;
    f.name = "Terrans";
    s.factions[f.id] = f;
  }

  // Two star systems.
  {
    StarSystem a;
    a.id = 10;
    a.name = "Alpha";
    s.systems[a.id] = a;
  }
  {
    StarSystem b;
    b.id = 20;
    b.name = "Beta";
    s.systems[b.id] = b;
  }

  // Connected jump points.
  {
    JumpPoint jp;
    jp.id = 100;
    jp.name = "JP-Alpha";
    jp.system_id = 10;
    jp.position_mkm = Vec2{0.0, 0.0};
    jp.linked_jump_id = 101;
    s.jump_points[jp.id] = jp;
    s.systems[jp.system_id].jump_points.push_back(jp.id);
  }
  {
    JumpPoint jp;
    jp.id = 101;
    jp.name = "JP-Beta";
    jp.system_id = 20;
    jp.position_mkm = Vec2{0.0, 0.0};
    jp.linked_jump_id = 100;
    s.jump_points[jp.id] = jp;
    s.systems[jp.system_id].jump_points.push_back(jp.id);
  }

  // Two ships in the same system but at different distances from the jump point.
  {
    Ship sh;
    sh.id = 1000;
    sh.name = "Fast";
    sh.faction_id = 1;
    sh.system_id = 10;
    sh.design_id = "fast";
    sh.position_mkm = Vec2{0.0, 0.0}; // already at the jump point
    s.ships[sh.id] = sh;
    s.systems[sh.system_id].ships.push_back(sh.id);
  }
  {
    Ship sh;
    sh.id = 1001;
    sh.name = "Slow";
    sh.faction_id = 1;
    sh.system_id = 10;
    sh.design_id = "slow";
    sh.position_mkm = Vec2{17.28, 0.0}; // 2 days @ 100 km/s (8.64 mkm/day)
    s.ships[sh.id] = sh;
    s.systems[sh.system_id].ships.push_back(sh.id);
  }

  s.selected_system = 10;
  return s;
}

nebula4x::GameState make_formation_state() {
  using namespace nebula4x;

  GameState s;
  s.save_version = GameState{}.save_version;
  s.date = Date::from_ymd(2200, 1, 1);
  s.next_id = 9000;

  // One faction.
  {
    Faction f;
    f.id = 1;
    f.name = "Terrans";
    s.factions[f.id] = f;
  }

  // One star system.
  {
    StarSystem sys;
    sys.id = 10;
    sys.name = "TestSys";
    s.systems[sys.id] = sys;
  }

  // Three ships in the same system.
  for (Id sid : std::vector<Id>{1000, 1001, 1002}) {
    Ship sh;
    sh.id = sid;
    sh.name = std::string("Ship-") + std::to_string(static_cast<unsigned long long>(sid));
    sh.faction_id = 1;
    sh.system_id = 10;
    sh.design_id = "fast";
    sh.position_mkm = Vec2{0.0, 0.0};
    s.ships[sh.id] = sh;
    s.systems[sh.system_id].ships.push_back(sh.id);
  }

  s.selected_system = 10;
  return s;
}

} // namespace

int test_fleets() {
  using namespace nebula4x;

  ContentDB content;
  Simulation sim(content, SimConfig{});
  sim.load_game(make_minimal_state());

  // --- Create fleet ---
  {
    std::string err;
    const Id fid = sim.create_fleet(1, "1st Fleet", std::vector<Id>{10, 11}, &err);
    N4X_ASSERT(fid != kInvalidId);
    N4X_ASSERT(err.empty());
    N4X_ASSERT(sim.state().fleets.size() == 1);

    const Fleet& fl = sim.state().fleets.at(fid);
    N4X_ASSERT(fl.faction_id == 1);
    N4X_ASSERT(fl.name == "1st Fleet");
    N4X_ASSERT(fl.leader_ship_id == 10);
    N4X_ASSERT(fl.ship_ids.size() == 2);
    N4X_ASSERT(sim.fleet_for_ship(10) == fid);
    N4X_ASSERT(sim.fleet_for_ship(11) == fid);
    N4X_ASSERT(sim.fleet_for_ship(12) == kInvalidId);

    // Creating another fleet using a ship already in a fleet should fail.
    err.clear();
    const Id bad = sim.create_fleet(1, "Dup", std::vector<Id>{10}, &err);
    N4X_ASSERT(bad == kInvalidId);
    N4X_ASSERT(!err.empty());

    // Adding a ship of the wrong faction should fail.
    err.clear();
    N4X_ASSERT(!sim.add_ship_to_fleet(fid, 12, &err));
    N4X_ASSERT(!err.empty());

    // Leader switch.
    N4X_ASSERT(sim.set_fleet_leader(fid, 11));
    N4X_ASSERT(sim.state().fleets.at(fid).leader_ship_id == 11);

    // Bulk order issuing: wait 3 days.
    N4X_ASSERT(sim.issue_fleet_wait_days(fid, 3));
    N4X_ASSERT(sim.state().ship_orders.at(10).queue.size() == 1);
    N4X_ASSERT(sim.state().ship_orders.at(11).queue.size() == 1);
    N4X_ASSERT(std::holds_alternative<WaitDays>(sim.state().ship_orders.at(10).queue.front()));

    // Clear fleet orders should clear both.
    N4X_ASSERT(sim.clear_fleet_orders(fid));
    N4X_ASSERT(sim.state().ship_orders.at(10).queue.empty());
    N4X_ASSERT(sim.state().ship_orders.at(11).queue.empty());

    // Remove members: removing last member should auto-disband.
    N4X_ASSERT(sim.remove_ship_from_fleet(fid, 10));
    N4X_ASSERT(sim.fleet_for_ship(10) == kInvalidId);
    // fleet still exists with ship 11
    N4X_ASSERT(sim.state().fleets.count(fid) == 1);
    N4X_ASSERT(sim.remove_ship_from_fleet(fid, 11));
    N4X_ASSERT(sim.state().fleets.empty());
  }

  // --- Serialization roundtrip ---
  {
    std::string err;
    const Id fid = sim.create_fleet(1, "SerializeMe", std::vector<Id>{10, 11}, &err);
    N4X_ASSERT(fid != kInvalidId);

    // Newer saves should persist optional formation settings.
    N4X_ASSERT(sim.configure_fleet_formation(fid, FleetFormation::Wedge, 2.5));

    // ...and fleet mission automation config / runtime.
    sim.state().fleets.at(fid).mission.type = FleetMissionType::AssaultColony;
    sim.state().fleets.at(fid).mission.patrol_system_id = 12345;
    sim.state().fleets.at(fid).mission.patrol_dwell_days = 7;
    sim.state().fleets.at(fid).mission.patrol_leg_index = 3;
    sim.state().fleets.at(fid).mission.hunt_max_contact_age_days = 31;
    sim.state().fleets.at(fid).mission.escort_target_ship_id = 777;
    sim.state().fleets.at(fid).mission.escort_active_ship_id = 778;
    sim.state().fleets.at(fid).mission.escort_follow_distance_mkm = 2.0;
    sim.state().fleets.at(fid).mission.escort_defense_radius_mkm = 123.0;
    sim.state().fleets.at(fid).mission.escort_only_auto_freight = false;
    sim.state().fleets.at(fid).mission.escort_retarget_interval_days = 9;
    sim.state().fleets.at(fid).mission.escort_last_retarget_day = 321;
    sim.state().fleets.at(fid).mission.auto_refuel = true;
    sim.state().fleets.at(fid).mission.refuel_threshold_fraction = 0.2;
    sim.state().fleets.at(fid).mission.refuel_resume_fraction = 0.95;
    sim.state().fleets.at(fid).mission.auto_repair = false;
    sim.state().fleets.at(fid).mission.repair_threshold_fraction = 0.4;
    sim.state().fleets.at(fid).mission.repair_resume_fraction = 0.9;
    sim.state().fleets.at(fid).mission.sustainment_mode = FleetSustainmentMode::Refuel;
    sim.state().fleets.at(fid).mission.sustainment_colony_id = 999;
    sim.state().fleets.at(fid).mission.last_target_ship_id = 4242;
    sim.state().fleets.at(fid).mission.blockade_colony_id = 1001;
    sim.state().fleets.at(fid).mission.blockade_radius_mkm = 77.7;
    sim.state().fleets.at(fid).mission.patrol_route_a_system_id = 2001;
    sim.state().fleets.at(fid).mission.patrol_route_b_system_id = 2002;
    sim.state().fleets.at(fid).mission.guard_jump_point_id = 3001;
    sim.state().fleets.at(fid).mission.guard_jump_radius_mkm = 55.5;
    sim.state().fleets.at(fid).mission.guard_jump_dwell_days = 6;
    sim.state().fleets.at(fid).mission.guard_last_alert_day = 1234;
    sim.state().fleets.at(fid).mission.patrol_circuit_system_ids = {4001, 4002, 4003};
    sim.state().fleets.at(fid).mission.patrol_region_id = 5001;
    sim.state().fleets.at(fid).mission.patrol_region_dwell_days = 2;
    sim.state().fleets.at(fid).mission.patrol_region_system_index = 1;
    sim.state().fleets.at(fid).mission.explore_survey_first = false;
    sim.state().fleets.at(fid).mission.explore_allow_transit = false;
    sim.state().fleets.at(fid).mission.explore_survey_transit_when_done = false;
    sim.state().fleets.at(fid).mission.explore_investigate_anomalies = false;
    sim.state().fleets.at(fid).mission.explore_salvage_wrecks = false;

    // Assault mission params should also round-trip.
    sim.state().fleets.at(fid).mission.assault_colony_id = 123;
    sim.state().fleets.at(fid).mission.assault_staging_colony_id = 456;
    sim.state().fleets.at(fid).mission.assault_auto_stage = false;
    sim.state().fleets.at(fid).mission.assault_troop_margin_factor = 1.23;
    sim.state().fleets.at(fid).mission.assault_use_bombardment = true;
    sim.state().fleets.at(fid).mission.assault_bombard_days = 42;
    sim.state().fleets.at(fid).mission.assault_bombard_executed = true;

    const std::string json_text = serialize_game_to_json(sim.state());
    const GameState loaded = deserialize_game_from_json(json_text);

    N4X_ASSERT(loaded.fleets.size() == 1);
    const auto it = loaded.fleets.begin();
    const Fleet& fl = it->second;
    N4X_ASSERT(fl.name == "SerializeMe");
    N4X_ASSERT(fl.faction_id == 1);
    N4X_ASSERT(fl.ship_ids.size() == 2);
    N4X_ASSERT(std::find(fl.ship_ids.begin(), fl.ship_ids.end(), 10) != fl.ship_ids.end());
    N4X_ASSERT(std::find(fl.ship_ids.begin(), fl.ship_ids.end(), 11) != fl.ship_ids.end());

    N4X_ASSERT(fl.formation == FleetFormation::Wedge);
    N4X_ASSERT(std::fabs(fl.formation_spacing_mkm - 2.5) < 1e-9);

    // Fleet mission should also round-trip.
    N4X_ASSERT(fl.mission.type == FleetMissionType::AssaultColony);
    N4X_ASSERT(fl.mission.patrol_system_id == 12345);
    N4X_ASSERT(fl.mission.patrol_dwell_days == 7);
    N4X_ASSERT(fl.mission.patrol_leg_index == 3);
    N4X_ASSERT(fl.mission.hunt_max_contact_age_days == 31);
    N4X_ASSERT(fl.mission.escort_target_ship_id == 777);
    N4X_ASSERT(fl.mission.escort_active_ship_id == 778);
    N4X_ASSERT(std::fabs(fl.mission.escort_follow_distance_mkm - 2.0) < 1e-9);
    N4X_ASSERT(std::fabs(fl.mission.escort_defense_radius_mkm - 123.0) < 1e-9);
    N4X_ASSERT(fl.mission.escort_only_auto_freight == false);
    N4X_ASSERT(fl.mission.escort_retarget_interval_days == 9);
    N4X_ASSERT(fl.mission.escort_last_retarget_day == 321);
    N4X_ASSERT(fl.mission.auto_refuel == true);
    N4X_ASSERT(std::fabs(fl.mission.refuel_threshold_fraction - 0.2) < 1e-9);
    N4X_ASSERT(std::fabs(fl.mission.refuel_resume_fraction - 0.95) < 1e-9);
    N4X_ASSERT(fl.mission.auto_repair == false);
    N4X_ASSERT(std::fabs(fl.mission.repair_threshold_fraction - 0.4) < 1e-9);
    N4X_ASSERT(std::fabs(fl.mission.repair_resume_fraction - 0.9) < 1e-9);
    N4X_ASSERT(fl.mission.sustainment_mode == FleetSustainmentMode::Refuel);
    N4X_ASSERT(fl.mission.sustainment_colony_id == 999);
    N4X_ASSERT(fl.mission.last_target_ship_id == 4242);
    N4X_ASSERT(fl.mission.blockade_colony_id == 1001);
    N4X_ASSERT(std::fabs(fl.mission.blockade_radius_mkm - 77.7) < 1e-9);
    N4X_ASSERT(fl.mission.patrol_route_a_system_id == 2001);
    N4X_ASSERT(fl.mission.patrol_route_b_system_id == 2002);
    N4X_ASSERT(fl.mission.guard_jump_point_id == 3001);
    N4X_ASSERT(std::fabs(fl.mission.guard_jump_radius_mkm - 55.5) < 1e-9);
    N4X_ASSERT(fl.mission.guard_jump_dwell_days == 6);
    N4X_ASSERT(fl.mission.guard_last_alert_day == 1234);
    N4X_ASSERT(fl.mission.patrol_circuit_system_ids.size() == 3);
    N4X_ASSERT(fl.mission.patrol_circuit_system_ids[0] == 4001);
    N4X_ASSERT(fl.mission.patrol_circuit_system_ids[1] == 4002);
    N4X_ASSERT(fl.mission.patrol_circuit_system_ids[2] == 4003);
    N4X_ASSERT(fl.mission.patrol_region_id == 5001);
    N4X_ASSERT(fl.mission.patrol_region_dwell_days == 2);
    N4X_ASSERT(fl.mission.patrol_region_system_index == 1);
    N4X_ASSERT(fl.mission.explore_survey_first == false);
    N4X_ASSERT(fl.mission.explore_allow_transit == false);
    N4X_ASSERT(fl.mission.explore_survey_transit_when_done == false);
    N4X_ASSERT(fl.mission.explore_investigate_anomalies == false);
    N4X_ASSERT(fl.mission.explore_salvage_wrecks == false);

    N4X_ASSERT(fl.mission.assault_colony_id == 123);
    N4X_ASSERT(fl.mission.assault_staging_colony_id == 456);
    N4X_ASSERT(fl.mission.assault_auto_stage == false);
    N4X_ASSERT(std::fabs(fl.mission.assault_troop_margin_factor - 1.23) < 1e-9);
    N4X_ASSERT(fl.mission.assault_use_bombardment == true);
    N4X_ASSERT(fl.mission.assault_bombard_days == 42);
    N4X_ASSERT(fl.mission.assault_bombard_executed == true);
  }


  // --- Fleet mission automation: defend colony engages hostiles ---
  {
    using namespace nebula4x;

    ContentDB content_defend;
    {
      ShipDesign d;
      d.id = "sensor";
      d.name = "Sensor";
      d.sensor_range_mkm = 1e9;
      d.speed_km_s = 100.0;
      d.max_hp = 10.0;
      content_defend.designs[d.id] = d;
    }

    Simulation sim_defend(content_defend, SimConfig{});
    sim_defend.load_game(make_defend_mission_state());

    // Give both sides basic sensors so the hostile is detectable.
    sim_defend.state().ships.at(10).design_id = "sensor";
    sim_defend.state().ships.at(11).design_id = "sensor";
    sim_defend.state().ships.at(12).design_id = "sensor";

    std::string err;
    const Id fid = sim_defend.create_fleet(1, "Defenders", std::vector<Id>{10, 11}, &err);
    N4X_ASSERT(fid != kInvalidId);

    auto& fl = sim_defend.state().fleets.at(fid);
    fl.mission.type = FleetMissionType::DefendColony;
    fl.mission.defend_colony_id = 500;
    fl.mission.auto_refuel = false;
    fl.mission.auto_repair = false;

    // Advance one day: fleet mission planning runs in tick_ai() and should
    // push an AttackShip order against the detected hostile.
    sim_defend.advance_days(1);

    const auto& q10 = sim_defend.state().ship_orders.at(10).queue;
    const auto& q11 = sim_defend.state().ship_orders.at(11).queue;
    N4X_ASSERT(!q10.empty());
    N4X_ASSERT(!q11.empty());
    N4X_ASSERT(std::holds_alternative<AttackShip>(q10.front()));
    N4X_ASSERT(std::holds_alternative<AttackShip>(q11.front()));
    N4X_ASSERT(std::get<AttackShip>(q10.front()).target_ship_id == 12);
    N4X_ASSERT(std::get<AttackShip>(q11.front()).target_ship_id == 12);
  }


  // --- Fleet mission automation: escort freighters ---
  {
    using namespace nebula4x;

    ContentDB content_escort;
    {
      ShipDesign d;
      d.id = "escort";
      d.name = "Escort";
      d.role = ShipRole::Combatant;
      d.sensor_range_mkm = 1e9;
      d.speed_km_s = 200.0;
      d.max_hp = 10.0;
      content_escort.designs[d.id] = d;
    }
    {
      ShipDesign d;
      d.id = "freighter";
      d.name = "Freighter";
      d.role = ShipRole::Freighter;
      d.speed_km_s = 100.0;
      d.max_hp = 10.0;
      content_escort.designs[d.id] = d;
    }
    {
      ShipDesign d;
      d.id = "hostile";
      d.name = "Hostile";
      d.role = ShipRole::Combatant;
      d.sensor_range_mkm = 1e9;
      d.speed_km_s = 100.0;
      d.max_hp = 10.0;
      content_escort.designs[d.id] = d;
    }

    Simulation sim_escort(content_escort, SimConfig{});
    sim_escort.load_game(make_escort_mission_state());

    sim_escort.state().ships.at(10).design_id = "escort";
    sim_escort.state().ships.at(11).design_id = "escort";
    sim_escort.state().ships.at(12).design_id = "hostile";
    sim_escort.state().ships.at(13).design_id = "freighter";

    std::string err;
    const Id fid = sim_escort.create_fleet(1, "Escorts", std::vector<Id>{10, 11}, &err);
    N4X_ASSERT(fid != kInvalidId);
    N4X_ASSERT(err.empty());

    auto& fl = sim_escort.state().fleets.at(fid);
    fl.mission.type = FleetMissionType::EscortFreighters;
    fl.mission.escort_target_ship_id = kInvalidId;  // auto
    fl.mission.escort_defense_radius_mkm = 10.0;
    fl.mission.auto_refuel = false;
    fl.mission.auto_repair = false;

    // Day 1: with the hostile far outside the defense radius, the fleet should escort.
    sim_escort.advance_days(1);
    const auto& q10 = sim_escort.state().ship_orders.at(10).queue;
    const auto& q11 = sim_escort.state().ship_orders.at(11).queue;
    N4X_ASSERT(!q10.empty());
    N4X_ASSERT(!q11.empty());
    N4X_ASSERT(std::holds_alternative<EscortShip>(q10.front()));
    N4X_ASSERT(std::holds_alternative<EscortShip>(q11.front()));
    N4X_ASSERT(std::get<EscortShip>(q10.front()).target_ship_id == 13);
    N4X_ASSERT(std::get<EscortShip>(q11.front()).target_ship_id == 13);

    // Move hostile close to the freighter: the escorts should immediately attack.
    sim_escort.state().ships.at(12).position_mkm = Vec2{5.0, 0.0};
    sim_escort.advance_days(1);

    const auto& q10b = sim_escort.state().ship_orders.at(10).queue;
    const auto& q11b = sim_escort.state().ship_orders.at(11).queue;
    N4X_ASSERT(!q10b.empty());
    N4X_ASSERT(!q11b.empty());
    N4X_ASSERT(std::holds_alternative<AttackShip>(q10b.front()));
    N4X_ASSERT(std::holds_alternative<AttackShip>(q11b.front()));
    N4X_ASSERT(std::get<AttackShip>(q10b.front()).target_ship_id == 12);
    N4X_ASSERT(std::get<AttackShip>(q11b.front()).target_ship_id == 12);
  }

  // --- Coordinated fleet jump transit ---
  {
    ContentDB content_jump;

    // Two simple designs with different speeds.
    {
      ShipDesign d;
      d.id = "fast";
      d.name = "Fast";
      d.speed_km_s = 200.0;
      d.max_hp = 10.0;
      content_jump.designs[d.id] = d;
    }
    {
      ShipDesign d;
      d.id = "slow";
      d.name = "Slow";
      d.speed_km_s = 100.0;
      d.max_hp = 10.0;
      content_jump.designs[d.id] = d;
    }

    Simulation sim_jump(content_jump, SimConfig{});
    sim_jump.load_game(make_jump_state());

    std::string err;
    const Id fid = sim_jump.create_fleet(1, "JumpFleet", std::vector<Id>{1000, 1001}, &err);
    N4X_ASSERT(fid != kInvalidId);
    N4X_ASSERT(err.empty());

    // Issue a fleet jump. The leader is already at the jump point.
    N4X_ASSERT(sim_jump.issue_fleet_travel_via_jump(fid, 100));

    // Day 1: leader should *not* jump yet, because the slower ship hasn't arrived.
    sim_jump.advance_days(1);
    N4X_ASSERT(sim_jump.state().ships.at(1000).system_id == 10);
    N4X_ASSERT(sim_jump.state().ships.at(1001).system_id == 10);
    N4X_ASSERT(!sim_jump.state().ship_orders.at(1000).queue.empty());
    N4X_ASSERT(std::holds_alternative<TravelViaJump>(sim_jump.state().ship_orders.at(1000).queue.front()));
    N4X_ASSERT(std::fabs(sim_jump.state().ships.at(1001).position_mkm.x - 8.64) < 1e-6);

    // Day 2: slower ship arrives; still no jump until the following day.
    sim_jump.advance_days(1);
    N4X_ASSERT(sim_jump.state().ships.at(1000).system_id == 10);
    N4X_ASSERT(sim_jump.state().ships.at(1001).system_id == 10);
    N4X_ASSERT(std::fabs(sim_jump.state().ships.at(1001).position_mkm.x) < 1e-6);
    N4X_ASSERT(!sim_jump.state().ship_orders.at(1000).queue.empty());
    N4X_ASSERT(std::holds_alternative<TravelViaJump>(sim_jump.state().ship_orders.at(1000).queue.front()));

    // Day 3: both ships are at the jump point at the start of the tick, so they
    // should transit together.
    sim_jump.advance_days(1);
    N4X_ASSERT(sim_jump.state().ships.at(1000).system_id == 20);
    N4X_ASSERT(sim_jump.state().ships.at(1001).system_id == 20);
    N4X_ASSERT(sim_jump.state().ship_orders.at(1000).queue.empty());
    N4X_ASSERT(sim_jump.state().ship_orders.at(1001).queue.empty());
  }

  // --- Fleet formations (move-to-point) ---
  {
    ContentDB content_form;

    // A very fast design so ships snap to targets in a single day.
    {
      ShipDesign d;
      d.id = "fast";
      d.name = "Fast";
      d.speed_km_s = 10000.0;
      d.max_hp = 10.0;
      content_form.designs[d.id] = d;
    }

    Simulation sim_form(content_form, SimConfig{});
    sim_form.load_game(make_formation_state());

    std::string err;
    const Id fid = sim_form.create_fleet(1, "FormFleet", std::vector<Id>{1000, 1001, 1002}, &err);
    N4X_ASSERT(fid != kInvalidId);
    N4X_ASSERT(err.empty());

    N4X_ASSERT(sim_form.configure_fleet_formation(fid, FleetFormation::LineAbreast, 2.0));
    N4X_ASSERT(sim_form.issue_fleet_move_to_point(fid, Vec2{100.0, 0.0}));

    sim_form.advance_days(1);

    const auto& a = sim_form.state().ships.at(1000);
    const auto& b = sim_form.state().ships.at(1001);
    const auto& c = sim_form.state().ships.at(1002);

    N4X_ASSERT(std::fabs(a.position_mkm.x - 100.0) < 1e-9);
    N4X_ASSERT(std::fabs(a.position_mkm.y - 0.0) < 1e-9);

    N4X_ASSERT(std::fabs(b.position_mkm.x - 100.0) < 1e-9);
    N4X_ASSERT(std::fabs(c.position_mkm.x - 100.0) < 1e-9);

    // With forward=(1,0) and right=(0,1), line-abreast offsets should be in +/-Y.
    N4X_ASSERT(std::fabs(std::fabs(b.position_mkm.y) - 2.0) < 1e-9);
    N4X_ASSERT(std::fabs(std::fabs(c.position_mkm.y) - 2.0) < 1e-9);
    N4X_ASSERT(b.position_mkm.y * c.position_mkm.y < 0.0);
  }


// Fleet explore mission: survey unknown exit, then transit into undiscovered space.
{
  ContentDB content_explore;

  // Provide the designs referenced by make_jump_state().
  {
    ShipDesign d;
    d.id = "fast";
    d.name = "Fast";
    d.speed_km_s = 10000.0;
    d.max_hp = 10.0;
    content_explore.designs[d.id] = d;
  }
  {
    ShipDesign d;
    d.id = "slow";
    d.name = "Slow";
    d.speed_km_s = 10.0;
    d.max_hp = 10.0;
    content_explore.designs[d.id] = d;
  }

  Simulation sim_explore(content_explore, SimConfig{});
  sim_explore.load_game(make_jump_state());
  auto& s = sim_explore.state();

  // Enable the new explore mission.
  auto& fl = s.fleets.at(1000);
  fl.mission.type = FleetMissionType::Explore;
  fl.mission.explore_survey_first = true;
  fl.mission.explore_allow_transit = true;

  // Ensure the local jump point starts as *unsurveyed* so we exercise the
  // survey-first behavior.
  auto& fac = s.factions.at(1);
  fac.surveyed_jump_points.clear();

  // Park both ships on top of the jump point so the survey completes instantly.
  s.ships.at(1).position_mkm = {0.0, 0.0};
  s.ships.at(2).position_mkm = {0.0, 0.0};

  // Day 1: mission should move to/survey the jump point, but not transit yet
  // (transit decision happens on the next AI tick).
  sim_explore.advance_days(1);
  N4X_ASSERT(s.ships.at(1).system_id == 10);
  N4X_ASSERT(s.ships.at(2).system_id == 10);
  N4X_ASSERT(sim_explore.is_jump_point_surveyed_by_faction(1, 100));

  // Day 2: jump point is now surveyed, so the mission should transit into the
  // undiscovered system.
  sim_explore.advance_days(1);
  N4X_ASSERT(s.ships.at(1).system_id == 20);
  N4X_ASSERT(s.ships.at(2).system_id == 20);
  N4X_ASSERT(sim_explore.is_system_discovered_by_faction(1, 20));
}

return 0;
}

