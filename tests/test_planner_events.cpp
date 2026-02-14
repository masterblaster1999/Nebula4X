#include <iostream>

#include "nebula4x/core/planner_events.h"
#include "nebula4x/core/simulation.h"

#define N4X_ASSERT(expr) \
  do { \
    if (!(expr)) { \
      std::cerr << "ASSERT failed: " #expr " (" << __FILE__ << ":" << __LINE__ << ")\n"; \
      return 1; \
    } \
  } while (0)

int test_planner_events() {
  using nebula4x::Body;
  using nebula4x::BuildOrder;
  using nebula4x::Colony;
  using nebula4x::ContentDB;
  using nebula4x::Date;
  using nebula4x::EventCategory;
  using nebula4x::Faction;
  using nebula4x::GameState;
  using nebula4x::Id;
  using nebula4x::InstallationBuildOrder;
  using nebula4x::InstallationDef;
  using nebula4x::PlannerEventsOptions;
  using nebula4x::PlannerEventsResult;
  using nebula4x::ShipDesign;
  using nebula4x::Simulation;
  using nebula4x::StarSystem;
  using nebula4x::TechDef;
  using nebula4x::compute_planner_events;

  ContentDB content;

  // Minimal tech + installations to drive a deterministic 1-day forecast.
  {
    InstallationDef lab;
    lab.id = "lab";
    lab.research_points_per_day = 10.0;
    content.installations[lab.id] = lab;

    InstallationDef shipyard;
    shipyard.id = "shipyard";
    shipyard.build_rate_tons_per_day = 100.0;
    content.installations[shipyard.id] = shipyard;

    InstallationDef factory;
    factory.id = "factory";
    factory.construction_points_per_day = 10.0;
    content.installations[factory.id] = factory;

    InstallationDef mine;
    mine.id = "mine";
    mine.construction_cost = 10.0;
    content.installations[mine.id] = mine;

    ShipDesign design;
    design.id = "design";
    design.name = "Test Design";
    design.mass_tons = 50.0;
    content.designs[design.id] = design;

    TechDef t;
    t.id = "test_tech";
    t.name = "Test Tech";
    t.cost = 10.0;
    content.techs[t.id] = t;
  }

  nebula4x::SimConfig cfg;
  cfg.enable_colony_stability_output_scaling = false;
  cfg.enable_colony_conditions = false;
  cfg.enable_trade_prosperity = false;
  cfg.enable_blockades = false;
  Simulation sim(std::move(content), cfg);

  GameState s;
  s.save_version = GameState{}.save_version;
  s.date = Date(0);
  s.hour_of_day = 0;
  sim.load_game(std::move(s));

  const Id fid = 1;
  {
    Faction f;
    f.id = fid;
    f.name = "Faction";
    f.research_queue.push_back("test_tech");
    sim.state().factions[fid] = f;
  }

  const Id sys_id = 10;
  {
    StarSystem sys;
    sys.id = sys_id;
    sys.name = "System";
    sim.state().systems[sys_id] = sys;
  }

  const Id body_id = 11;
  {
    Body b;
    b.id = body_id;
    b.name = "Body";
    b.system_id = sys_id;
    sim.state().bodies[body_id] = b;
  }

  const Id colony_id = 12;
  {
    Colony c;
    c.id = colony_id;
    c.name = "Colony";
    c.faction_id = fid;
    c.body_id = body_id;

    c.installations["lab"] = 1;
    c.installations["shipyard"] = 1;
    c.installations["factory"] = 1;

    BuildOrder bo;
    bo.design_id = "design";
    bo.tons_remaining = 50.0; // completes in 1 day with 100t/day
    c.shipyard_queue.push_back(bo);

    InstallationBuildOrder ib;
    ib.installation_id = "mine";
    ib.quantity_remaining = 1; // completes in 1 day with 10 CP/day
    c.construction_queue.push_back(ib);

    sim.state().colonies[colony_id] = c;
  }

  PlannerEventsOptions opt;
  opt.max_days = 30;
  opt.max_items = 64;
  opt.include_research = true;
  opt.include_colonies = true;
  opt.include_ships = false;
  opt.include_terraforming = false;

  PlannerEventsResult res = compute_planner_events(sim, fid, opt);
  N4X_ASSERT(res.ok);
  N4X_ASSERT(!res.items.empty());
  N4X_ASSERT(res.items.size() == 3);

  // Deterministic category ordering: Research, then Shipyard, then Construction
  // when all complete at the same time.
  N4X_ASSERT(res.items[0].category == EventCategory::Research);
  N4X_ASSERT(res.items[1].category == EventCategory::Shipyard);
  N4X_ASSERT(res.items[2].category == EventCategory::Construction);

  for (const auto& ev : res.items) {
    N4X_ASSERT(ev.eta_days == 1.0);
    N4X_ASSERT(ev.day == 1);
    N4X_ASSERT(ev.hour == 0);
  }

  return 0;
}
