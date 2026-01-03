#include <cmath>
#include <iostream>
#include <string>
#include <variant>

#include "nebula4x/core/freight_planner.h"
#include "nebula4x/core/simulation.h"


#define N4X_ASSERT(cond, msg)                                                       \
  do {                                                                              \
    if (!(cond)) {                                                                  \
      std::cerr << "ASSERT FAIL: " << (msg) << " (" << __FILE__ << ":" << __LINE__ \
                << ")\n";                                                         \
      return 1;                                                                     \
    }                                                                               \
  } while (0)

namespace {

double get_mineral(const nebula4x::Colony& c, const std::string& mineral) {
  auto it = c.minerals.find(mineral);
  return (it == c.minerals.end()) ? 0.0 : it->second;
}

template <typename T>
bool has_order_variant(const nebula4x::ShipOrders& so) {
  for (const auto& o : so.queue) {
    if (std::holds_alternative<T>(o)) return true;
  }
  return false;
}

}  // namespace

int test_freight_planner() {
  using namespace nebula4x;

  // Minimal content: a shipyard installation with a clean per-ton mineral cost.
  ContentDB content;

  InstallationDef yard;
  yard.id = "shipyard";
  yard.name = "Shipyard";
  yard.build_rate_tons_per_day = 100.0;
  yard.build_costs_per_ton["Duranium"] = 1.0;
  content.installations[yard.id] = yard;

  // Target design for the shipyard queue.
  ShipDesign target;
  target.id = "target";
  target.name = "Target";
  target.role = ShipRole::Combatant;
  target.mass_tons = 100.0;
  target.max_hp = 100.0;
  target.speed_km_s = 0.0;
  content.designs[target.id] = target;

  // Freighter design with cargo.
  ShipDesign freighter;
  freighter.id = "freighter";
  freighter.name = "Freighter";
  freighter.role = ShipRole::Freighter;
  freighter.mass_tons = 100.0;
  freighter.max_hp = 100.0;
  freighter.speed_km_s = 100.0;
  freighter.cargo_tons = 500.0;
  content.designs[freighter.id] = freighter;

  SimConfig cfg;
  cfg.auto_freight_min_transfer_tons = 1.0;
  cfg.auto_freight_max_take_fraction_of_surplus = 1.0;
  cfg.auto_freight_multi_mineral = true;

  Simulation sim(content, cfg);
  sim.new_game();

  GameState st = sim.state();
  N4X_ASSERT(!st.factions.empty(), "new_game should create a faction");
  const Faction f = st.factions.begin()->second;

  // One system.
  StarSystem sys;
  sys.id = 1;
  sys.name = "Sol";
  sys.galaxy_pos = Vec2{0.0, 0.0};
  st.systems[sys.id] = sys;

  // Two bodies at the same position (so travel is trivial).
  Body src_body;
  src_body.id = 10;
  src_body.name = "Source";
  src_body.system_id = sys.id;
  src_body.orbit_radius_mkm = 0.0;
  src_body.orbit_period_days = 1.0;
  src_body.orbit_phase_radians = 0.0;
  st.bodies[src_body.id] = src_body;

  Body dst_body;
  dst_body.id = 11;
  dst_body.name = "Dest";
  dst_body.system_id = sys.id;
  dst_body.orbit_radius_mkm = 0.0;
  dst_body.orbit_period_days = 1.0;
  dst_body.orbit_phase_radians = 0.0;
  st.bodies[dst_body.id] = dst_body;

  // Source colony has minerals.
  Colony src;
  src.id = 20;
  src.name = "Earth";
  src.faction_id = f.id;
  src.body_id = src_body.id;
  src.population_millions = 1000.0;
  src.minerals["Duranium"] = 1000.0;
  st.colonies[src.id] = src;

  // Dest colony has a shipyard queue but no minerals.
  Colony dst;
  dst.id = 21;
  dst.name = "Mars";
  dst.faction_id = f.id;
  dst.body_id = dst_body.id;
  dst.population_millions = 100.0;
  dst.installations["shipyard"] = 1;
  dst.shipyard_queue.push_back(BuildOrder{.design_id = target.id, .tons_remaining = 100.0});
  st.colonies[dst.id] = dst;

  // Idle auto-freight ship.
  Ship sh;
  sh.id = 100;
  sh.name = "Cargo-1";
  sh.faction_id = f.id;
  sh.design_id = freighter.id;
  sh.system_id = sys.id;
  sh.position_mkm = Vec2{0.0, 0.0};
  sh.auto_freight = true;
  st.ships[sh.id] = sh;

  sim.load_game(st);

  // --- 1) Basic: planner recommends hauling Duranium from src -> dst.
  {
    FreightPlannerOptions opt;
    opt.require_auto_freight_flag = true;
    opt.require_idle = true;
    opt.restrict_to_discovered = false;

    const auto plan = compute_freight_plan(sim, f.id, opt);
    N4X_ASSERT(plan.ok, "plan ok");
    N4X_ASSERT(!plan.assignments.empty(), "expected at least one assignment");

    const FreightAssignment& asg = plan.assignments.front();
    N4X_ASSERT(asg.ship_id == sh.id, "assignment targets the freighter");
    N4X_ASSERT(asg.kind == FreightAssignmentKind::PickupAndDeliver, "expected pickup+deliver");
    N4X_ASSERT(asg.source_colony_id == src.id, "source colony chosen");
    N4X_ASSERT(asg.dest_colony_id == dst.id, "dest colony chosen");
    N4X_ASSERT(asg.items.size() == 1, "one mineral item");
    N4X_ASSERT(asg.items[0].mineral == "Duranium", "Duranium selected");
    N4X_ASSERT(std::abs(asg.items[0].tons - 100.0) < 1e-6, "ships 100t to satisfy shipyard daily need");

    // Apply the assignment and verify that load/unload orders were queued.
    const bool applied = apply_freight_assignment(sim, asg, /*clear_existing_orders=*/true);
    N4X_ASSERT(applied, "apply_freight_assignment ok");

    const ShipOrders* so = find_ptr(sim.state().ship_orders, sh.id);
    N4X_ASSERT(so && !so->queue.empty(), "orders queued");
    N4X_ASSERT(has_order_variant<nebula4x::LoadMineral>(*so), "queued LoadMineral");
    N4X_ASSERT(has_order_variant<nebula4x::UnloadMineral>(*so), "queued UnloadMineral");

    // Clear orders to keep later subtests independent.
    (void)sim.clear_orders(sh.id);
  }

  // --- 2) Reserves cap: planner should not export below manual reserves.
  {
    GameState st2 = sim.state();
    st2.colonies[src.id].mineral_reserves["Duranium"] = 950.0;

    Simulation sim2(content, cfg);
    sim2.load_game(st2);

    FreightPlannerOptions opt;
    opt.require_auto_freight_flag = true;
    opt.require_idle = true;
    opt.restrict_to_discovered = false;

    const auto plan = compute_freight_plan(sim2, f.id, opt);
    N4X_ASSERT(plan.ok, "plan ok (reserves)");
    N4X_ASSERT(!plan.assignments.empty(), "assignment exists (reserves)");

    const auto& asg = plan.assignments.front();
    N4X_ASSERT(asg.items.size() == 1, "one mineral item");
    N4X_ASSERT(asg.items[0].mineral == "Duranium", "Duranium selected (reserves)");
    N4X_ASSERT(std::abs(asg.items[0].tons - 50.0) < 1e-6, "export capped at surplus above reserve");

    // Sanity: we didn't mutate minerals by planning.
    N4X_ASSERT(std::abs(get_mineral(sim2.state().colonies.at(src.id), "Duranium") - 1000.0) < 1e-6,
               "planning does not mutate colony minerals");
  }

  // --- 3) Multi-mineral bundling: if multiple minerals are missing, planner bundles them.
  {
    ContentDB content3 = content;
    content3.installations[yard.id].build_costs_per_ton["Corbomite"] = 1.0;

    Simulation sim3(content3, cfg);
    GameState st3 = sim.state();
    st3.colonies[src.id].minerals["Corbomite"] = 1000.0;
    sim3.load_game(st3);

    FreightPlannerOptions opt;
    opt.require_auto_freight_flag = true;
    opt.require_idle = true;
    opt.restrict_to_discovered = false;

    const auto plan = compute_freight_plan(sim3, f.id, opt);
    N4X_ASSERT(plan.ok, "plan ok (bundling)");
    N4X_ASSERT(!plan.assignments.empty(), "assignment exists (bundling)");

    const auto& asg = plan.assignments.front();
    N4X_ASSERT(asg.items.size() == 2, "bundled 2 mineral items");

    // The shipyard build_rate is 100 t/day and both minerals cost 1/t.
    double dur = 0.0;
    double cor = 0.0;
    for (const auto& it : asg.items) {
      if (it.mineral == "Duranium") dur = it.tons;
      if (it.mineral == "Corbomite") cor = it.tons;
    }
    N4X_ASSERT(std::abs(dur - 100.0) < 1e-6, "bundled Duranium 100");
    N4X_ASSERT(std::abs(cor - 100.0) < 1e-6, "bundled Corbomite 100");
  }

  // --- 4) require_auto_freight_flag filters ships.
  {
    GameState st4 = sim.state();
    st4.ships[sh.id].auto_freight = false;

    Simulation sim4(content, cfg);
    sim4.load_game(st4);

    FreightPlannerOptions opt;
    opt.require_auto_freight_flag = true;
    opt.require_idle = true;
    opt.restrict_to_discovered = false;

    const auto plan = compute_freight_plan(sim4, f.id, opt);
    N4X_ASSERT(plan.ok, "plan ok (filtered)");
    N4X_ASSERT(plan.assignments.empty(), "no assignments when ship is not auto_freight");

    opt.require_auto_freight_flag = false;
    const auto plan2 = compute_freight_plan(sim4, f.id, opt);
    N4X_ASSERT(plan2.ok, "plan ok (unfiltered)");
    N4X_ASSERT(!plan2.assignments.empty(), "assignment exists when not filtering auto_freight");
  }

  return 0;
}
