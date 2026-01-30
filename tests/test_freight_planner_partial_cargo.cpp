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

template <typename T>
const T* first_order_of(const nebula4x::ShipOrders& so) {
  for (const auto& o : so.queue) {
    if (const auto* p = std::get_if<T>(&o)) return p;
  }
  return nullptr;
}

}  // namespace

int test_freight_planner_partial_cargo() {
  using namespace nebula4x;

  // Minimal content: a shipyard installation with a per-ton mineral cost.
  ContentDB content;

  InstallationDef yard;
  yard.id = "shipyard";
  yard.name = "Shipyard";
  yard.build_rate_tons_per_day = 500.0;
  yard.build_costs_per_ton["Duranium"] = 1.0;
  content.installations[yard.id] = yard;

  // Target design for the shipyard queue.
  ShipDesign target;
  target.id = "target";
  target.name = "Target";
  target.role = ShipRole::Combatant;
  target.mass_tons = 500.0;
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

  // Source colony has minerals. We intentionally set this to 800 because the ship
  // already starts with 200t in cargo, keeping the test resource-conservative.
  Colony src;
  src.id = 20;
  src.name = "Earth";
  src.faction_id = f.id;
  src.body_id = src_body.id;
  src.population_millions = 1000.0;
  src.minerals["Duranium"] = 800.0;
  st.colonies[src.id] = src;

  // Dest colony has a shipyard queue but no minerals.
  Colony dst;
  dst.id = 21;
  dst.name = "Mars";
  dst.faction_id = f.id;
  dst.body_id = dst_body.id;
  dst.population_millions = 100.0;
  dst.installations["shipyard"] = 1;
  dst.shipyard_queue.push_back(BuildOrder{.design_id = target.id,
                                         .tons_remaining = 500.0,
                                         .apply_ship_profile_name = "",
                                         .assign_to_fleet_id = kInvalidId});
  st.colonies[dst.id] = dst;

  // Auto-freight ship that is already partially loaded with Duranium.
  Ship sh;
  sh.id = 100;
  sh.name = "Cargo-1";
  sh.faction_id = f.id;
  sh.design_id = freighter.id;
  sh.system_id = sys.id;
  sh.position_mkm = Vec2{0.0, 0.0};
  sh.auto_freight = true;
  sh.cargo["Duranium"] = 200.0;
  st.ships[sh.id] = sh;

  sim.load_game(st);

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

  // Key behavior: the plan should deliver the full 500t needed by the shipyard,
  // not just the 200t already on board.
  N4X_ASSERT(std::abs(asg.items[0].tons - 500.0) < 1e-6,
             "partially-loaded freighter should top-up and deliver 500t total");

  // Apply the assignment and verify that load/unload orders were queued with
  // correct tonnages (load 300, unload 500).
  const bool applied = apply_freight_assignment(sim, asg, /*clear_existing_orders=*/true);
  N4X_ASSERT(applied, "apply_freight_assignment ok");

  const ShipOrders* so = find_ptr(sim.state().ship_orders, sh.id);
  N4X_ASSERT(so && !so->queue.empty(), "orders queued");

  const auto* lo = first_order_of<LoadMineral>(*so);
  const auto* uo = first_order_of<UnloadMineral>(*so);
  N4X_ASSERT(lo, "queued LoadMineral");
  N4X_ASSERT(uo, "queued UnloadMineral");
  N4X_ASSERT(lo->colony_id == src.id, "LoadMineral from source colony");
  N4X_ASSERT(uo->colony_id == dst.id, "UnloadMineral to dest colony");
  N4X_ASSERT(lo->mineral == "Duranium", "load Duranium");
  N4X_ASSERT(uo->mineral == "Duranium", "unload Duranium");
  N4X_ASSERT(std::abs(lo->tons - 300.0) < 1e-6, "load 300t (top-up)");
  N4X_ASSERT(std::abs(uo->tons - 500.0) < 1e-6, "unload 500t (cargo+top-up)");

  // Execute one day: with co-located bodies, load+unload should complete.
  sim.advance_days(1);

  const GameState st_after = sim.state();
  const Colony& src_after = st_after.colonies.at(src.id);
  const Colony& dst_after = st_after.colonies.at(dst.id);
  const Ship& sh_after = st_after.ships.at(sh.id);

  const double src_dur = src_after.minerals.at("Duranium");
  const double dst_dur = dst_after.minerals.at("Duranium");
  double ship_dur = 0.0;
  if (auto it = sh_after.cargo.find("Duranium"); it != sh_after.cargo.end()) {
    ship_dur = it->second;
  }

  N4X_ASSERT(std::abs(src_dur - 500.0) < 1e-6, "source spent 300t (800 -> 500)");
  N4X_ASSERT(std::abs(dst_dur - 500.0) < 1e-6, "dest received 500t");
  N4X_ASSERT(std::abs(ship_dur - 0.0) < 1e-6, "ship unloaded all Duranium");

  return 0;
}
