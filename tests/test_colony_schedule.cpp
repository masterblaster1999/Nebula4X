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

  return 0;
}
