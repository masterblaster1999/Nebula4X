#include <cmath>
#include <iostream>

#include "nebula4x/core/colony_schedule.h"
#include "nebula4x/core/simulation.h"

#define N4X_ASSERT(expr)                                                                            \
  do {                                                                                              \
    if (!(expr)) {                                                                                  \
      std::cerr << "ASSERT failed: " #expr " (" << __FILE__ << ":" << __LINE__ << ")\n";            \
      return 1;                                                                                     \
    }                                                                                               \
  } while (0)

int test_colony_schedule() {
  using namespace nebula4x;

  ContentDB content;

  // Shipyard installation (consumes duranium per ton).
  {
    InstallationDef def;
    def.id = "shipyard";
    def.name = "Shipyard";
    def.build_rate_tons_per_day = 100.0;
    def.build_costs_per_ton["Duranium"] = 1.0;
    content.installations[def.id] = def;
  }

  // Construction installation.
  {
    InstallationDef def;
    def.id = "construction";
    def.name = "Construction Center";
    def.construction_points_per_day = 10.0;
    content.installations[def.id] = def;
  }

  // A mine that produces duranium, but costs corbomite to build.
  {
    InstallationDef def;
    def.id = "duranium_mine";
    def.name = "Duranium Mine";
    def.mining = true;
    def.produces_per_day["Duranium"] = 50.0;
    def.construction_cost = 10.0;
    def.build_costs["Corbomite"] = 10.0;
    content.installations[def.id] = def;
  }

  // A simple ship design.
  {
    ShipDesign d;
    d.id = "scout";
    d.name = "Scout";
    d.mass_tons = 100.0;
    content.designs[d.id] = d;
  }

  SimConfig cfg;
  cfg.seconds_per_day = 86400.0;
  Simulation sim(content, cfg);
  auto& st = sim.state();
  st.date = Date(0);
  st.hour_of_day = 0;

  // Minimal system/body.
  StarSystem sys;
  sys.id = 1;
  sys.name = "Sys";
  st.systems[sys.id] = sys;

  Body b;
  b.id = 10;
  b.system_id = sys.id;
  b.name = "World";
  b.parent_body_id = kInvalidId;
  b.mineral_deposits["Duranium"] = 1000.0;
  st.bodies[b.id] = b;

  // Faction (unlock the mine for auto-target tests).
  Faction f;
  f.id = 1;
  f.name = "Faction";
  f.unlocked_installations.push_back("duranium_mine");
  st.factions[f.id] = f;

  // --- Case 1: manual queue builds a mine on day 1; ship finishes on day 3. ---
  {
    Colony c;
    c.id = 100;
    c.name = "Col";
    c.body_id = b.id;
    c.faction_id = f.id;
    c.installations["shipyard"] = 1;
    c.installations["construction"] = 1;
    c.minerals["Corbomite"] = 10.0;
    c.minerals["Duranium"] = 0.0;

    InstallationBuildOrder co;
    co.installation_id = "duranium_mine";
    co.quantity_remaining = 1;
    c.construction_queue.push_back(co);

    BuildOrder bo;
    bo.design_id = "scout";
    bo.tons_remaining = 100.0;
    c.shipyard_queue.push_back(bo);

    st.colonies[c.id] = c;

    ColonyScheduleOptions opt;
    opt.max_days = 10;
    opt.max_events = 16;
    opt.include_auto_construction_targets = true;
    opt.include_shipyard = true;
    opt.include_construction = true;

    const ColonySchedule sched = estimate_colony_schedule(sim, c.id, opt);
    N4X_ASSERT(sched.ok);
    N4X_ASSERT(!sched.stalled);
    N4X_ASSERT(!sched.events.empty());

    // Expect two completion events: mine (day 1) and ship (day 3).
    N4X_ASSERT(sched.events.size() >= 2);
    N4X_ASSERT(sched.events[0].kind == ColonyScheduleEventKind::ConstructionComplete);
    N4X_ASSERT(sched.events[0].day == 1);
    N4X_ASSERT(sched.events[1].kind == ColonyScheduleEventKind::ShipyardComplete);
    N4X_ASSERT(sched.events[1].day == 3);
  }

  // --- Case 2: auto-target queues a mine and completes it on day 1. ---
  {
    Colony c;
    c.id = 101;
    c.name = "Col2";
    c.body_id = b.id;
    c.faction_id = f.id;
    c.installations["construction"] = 1;
    c.minerals["Corbomite"] = 10.0;
    c.installation_targets["duranium_mine"] = 1;
    st.colonies[c.id] = c;

    ColonyScheduleOptions opt;
    opt.max_days = 5;
    opt.max_events = 8;
    opt.include_auto_construction_targets = true;
    opt.include_shipyard = false;
    opt.include_construction = true;

    const ColonySchedule sched = estimate_colony_schedule(sim, c.id, opt);
    N4X_ASSERT(sched.ok);
    N4X_ASSERT(!sched.stalled);
    N4X_ASSERT(!sched.events.empty());
    N4X_ASSERT(sched.events[0].kind == ColonyScheduleEventKind::ConstructionComplete);
    N4X_ASSERT(sched.events[0].day == 1);
    N4X_ASSERT(sched.events[0].auto_queued);
  }

  // --- Case 3: unmet auto-target for a locked installation produces a stall reason. ---
  {
    Colony c;
    c.id = 102;
    c.name = "Col3";
    c.body_id = b.id;
    c.faction_id = f.id;
    c.installations["construction"] = 1;
    c.installation_targets["locked_inst"] = 1;
    st.colonies[c.id] = c;

    ColonyScheduleOptions opt;
    opt.max_days = 5;
    opt.max_events = 8;
    opt.include_auto_construction_targets = true;
    opt.include_shipyard = false;
    opt.include_construction = true;

    const ColonySchedule sched = estimate_colony_schedule(sim, c.id, opt);
    N4X_ASSERT(sched.ok);
    N4X_ASSERT(sched.stalled);
  }

  // --- Case 4: a stalled refit should not block later shipyard orders in the schedule. ---
  {
    // A ship that is NOT docked at the colony.
    Ship sh;
    sh.id = 150;
    sh.name = "Remote";
    sh.faction_id = f.id;
    sh.design_id = "scout";
    sh.system_id = sys.id;
    sh.position_mkm = Vec2{100.0, 0.0};
    st.ships[sh.id] = sh;

    Colony c;
    c.id = 103;
    c.name = "Col4";
    c.body_id = b.id;
    c.faction_id = f.id;
    c.installations["shipyard"] = 1;
    c.minerals["Duranium"] = 1000.0;

    // Front order: stalled refit (ship not docked).
    BuildOrder refit;
    refit.design_id = "scout";
    refit.refit_ship_id = sh.id;
    refit.tons_remaining = 50.0;
    c.shipyard_queue.push_back(refit);

    // Second order: should complete on day 1.
    BuildOrder build;
    build.design_id = "scout";
    build.tons_remaining = 100.0;
    c.shipyard_queue.push_back(build);

    st.colonies[c.id] = c;

    ColonyScheduleOptions opt;
    opt.max_days = 1;
    opt.max_events = 8;
    opt.include_auto_construction_targets = false;
    opt.include_shipyard = true;
    opt.include_construction = false;

    const ColonySchedule sched = estimate_colony_schedule(sim, c.id, opt);
    N4X_ASSERT(sched.ok);
    N4X_ASSERT(!sched.stalled);
    N4X_ASSERT(sched.events.size() == 1);
    N4X_ASSERT(sched.events[0].kind == ColonyScheduleEventKind::ShipyardComplete);
    N4X_ASSERT(sched.events[0].day == 1);
  }


  // --- Case 5: Generic mining (mining_tons_per_day) should feed shipyard builds in the schedule. ---
  // This validates that the colony schedule simulation matches the modern mining model used in
  // Simulation::tick_colonies (capacity distributed across all deposits by remaining composition).
  {
    ContentDB content2;

    // Shipyard consumes two minerals per ton.
    {
      InstallationDef shipyard;
      shipyard.id = "shipyard";
      shipyard.name = "Shipyard";
      shipyard.build_rate_tons_per_day = 100.0;
      shipyard.build_costs_per_ton = {{"Duranium", 1.0}, {"Tritanium", 1.0}};
      content2.installations[shipyard.id] = shipyard;
    }

    // Generic mine: no fixed produces_per_day; output is derived from deposits via mining_tons_per_day.
    {
      InstallationDef mine;
      mine.id = "automated_mine";
      mine.name = "Generic Mine";
      mine.mining = true;
      mine.mining_tons_per_day = 100.0;
      content2.installations[mine.id] = mine;
    }

    SimConfig cfg2;
    cfg2.seconds_per_day = 60.0;

    Simulation sim2(content2, cfg2);
    GameState& st2 = sim2.state();

    Faction f2;
    f2.id = 201;
    f2.name = "F2";
    st2.factions[f2.id] = f2;

    Body b2;
    b2.id = 202;
    b2.name = "B2";
    b2.mineral_deposits = {{"Duranium", 1000.0}, {"Tritanium", 1000.0}};
    st2.bodies[b2.id] = b2;

    Colony c2;
    c2.id = 203;
    c2.name = "Col4";
    c2.body_id = b2.id;
    c2.faction_id = f2.id;
    c2.installations["shipyard"] = 1;
    c2.installations["automated_mine"] = 1;

    BuildOrder ord;
    ord.design_id = "test_ship";
    ord.tons_remaining = 100.0;
    ord.auto_queued = false;
    c2.shipyard_queue.push_back(ord);

    st2.colonies[c2.id] = c2;

    ColonyScheduleOptions opt;
    opt.max_days = 10;
    opt.max_events = 8;
    opt.include_shipyard = true;
    opt.include_construction = false;

    const ColonySchedule sched = estimate_colony_schedule(sim2, c2.id, opt);
    N4X_ASSERT(sched.ok);
    N4X_ASSERT(!sched.stalled);
    N4X_ASSERT(sched.events.size() == 1);
    N4X_ASSERT(sched.events[0].kind == ColonyScheduleEventKind::ShipyardComplete);
    N4X_ASSERT(sched.events[0].day == 2);

    // Minerals should have been mined then immediately consumed by the shipyard build.
    auto it_d = sched.minerals_end.find("Duranium");
    auto it_t = sched.minerals_end.find("Tritanium");
    const double d_end = (it_d == sched.minerals_end.end()) ? 0.0 : it_d->second;
    const double t_end = (it_t == sched.minerals_end.end()) ? 0.0 : it_t->second;
    N4X_ASSERT(std::abs(d_end) < 1e-6);
    N4X_ASSERT(std::abs(t_end) < 1e-6);
  }


  // Colony conditions should affect schedule multipliers (and therefore completion times).
  {
    ContentDB content3;

    // Shipyard consumes two minerals per ton.
    {
      InstallationDef shipyard;
      shipyard.id = "shipyard";
      shipyard.name = "Shipyard";
      shipyard.build_rate_tons_per_day = 100.0;
      shipyard.build_costs_per_ton = {{"Duranium", 1.0}, {"Tritanium", 1.0}};
      content3.installations[shipyard.id] = shipyard;
    }

    // Generic mine: no fixed produces_per_day; output is derived from deposits via mining_tons_per_day.
    {
      InstallationDef mine;
      mine.id = "automated_mine";
      mine.name = "Generic Mine";
      mine.mining = true;
      mine.mining_tons_per_day = 100.0;
      content3.installations[mine.id] = mine;
    }

    SimConfig cfg3;
    cfg3.seconds_per_day = 60.0;
    cfg3.enable_colony_conditions = true;

    Simulation sim3(content3, cfg3);
    GameState& st3 = sim3.state();

    Faction f3;
    f3.id = 301;
    f3.name = "F3";
    st3.factions[f3.id] = f3;

    Body b3;
    b3.id = 302;
    b3.name = "B3";
    b3.mineral_deposits = {{"Duranium", 1000.0}, {"Tritanium", 1000.0}};
    st3.bodies[b3.id] = b3;

    Colony c3;
    c3.id = 303;
    c3.name = "ColCond";
    c3.body_id = b3.id;
    c3.faction_id = f3.id;
    c3.installations["shipyard"] = 1;
    c3.installations["automated_mine"] = 1;

    BuildOrder ord;
    ord.design_id = "test_ship";
    ord.tons_remaining = 100.0;
    ord.auto_queued = false;
    c3.shipyard_queue.push_back(ord);

    st3.colonies[c3.id] = c3;

    ColonyScheduleOptions opt;
    opt.max_days = 10;
    opt.max_events = 8;
    opt.include_shipyard = true;
    opt.include_construction = false;

    // Baseline: should match the earlier shipyard+mine test case (complete on day 2 due to mineral throttling).
    const ColonySchedule sched_base = estimate_colony_schedule(sim3, c3.id, opt);
    N4X_ASSERT(sched_base.ok);
    N4X_ASSERT(!sched_base.stalled);
    N4X_ASSERT(sched_base.events.size() == 1);
    N4X_ASSERT(sched_base.events[0].kind == ColonyScheduleEventKind::ShipyardComplete);
    N4X_ASSERT(sched_base.events[0].day == 2);

    // Apply a strike (shipyard multiplier 0.25, mining multiplier 0.75).
    Colony* c3p = find_ptr(st3.colonies, c3.id);
    N4X_ASSERT(c3p != nullptr);
    ColonyCondition strike;
    strike.id = "strike";
    strike.remaining_days = 30.0;
    strike.severity = 1.0;
    c3p->conditions.push_back(strike);

    const ColonySchedule sched_strike = estimate_colony_schedule(sim3, c3.id, opt);
    N4X_ASSERT(sched_strike.ok);
    N4X_ASSERT(!sched_strike.stalled);
    N4X_ASSERT(sched_strike.events.size() == 1);
    N4X_ASSERT(sched_strike.events[0].kind == ColonyScheduleEventKind::ShipyardComplete);

    // With the strike, the shipyard can only build 25t/day (and we mine 75t/day total), so completion should be day 4.
    N4X_ASSERT(sched_strike.events[0].day == 4);

    // Sanity: multipliers should reflect the condition.
    N4X_ASSERT(std::abs(sched_strike.shipyard_multiplier - sched_base.shipyard_multiplier * 0.25) < 1e-6);
    N4X_ASSERT(std::abs(sched_strike.mining_multiplier - sched_base.mining_multiplier * 0.75) < 1e-6);
  }

  return 0;
}
