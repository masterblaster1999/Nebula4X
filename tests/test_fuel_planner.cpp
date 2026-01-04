#include <cmath>
#include <iostream>
#include <string>
#include <variant>

#include "nebula4x/core/fuel_planner.h"
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
int count_order_variant(const nebula4x::ShipOrders& so) {
  int n = 0;
  for (const auto& o : so.queue) {
    if (std::holds_alternative<T>(o)) ++n;
  }
  return n;
}

}  // namespace

int test_fuel_planner() {
  using namespace nebula4x;

  ContentDB content;

  // Tanker design.
  ShipDesign tanker;
  tanker.id = "tanker";
  tanker.name = "Tanker";
  tanker.role = ShipRole::Freighter;
  tanker.mass_tons = 1000.0;
  tanker.max_hp = 100.0;
  tanker.speed_km_s = 100.0;
  tanker.fuel_capacity_tons = 1000.0;
  content.designs[tanker.id] = tanker;

  // Target design (fuel-hungry ship).
  ShipDesign target;
  target.id = "target";
  target.name = "Target";
  target.role = ShipRole::Combatant;
  target.mass_tons = 200.0;
  target.max_hp = 100.0;
  target.speed_km_s = 50.0;
  target.fuel_capacity_tons = 200.0;
  content.designs[target.id] = target;

  SimConfig cfg;
  cfg.auto_tanker_request_threshold_fraction = 0.25;
  cfg.auto_tanker_fill_target_fraction = 0.90;
  cfg.auto_tanker_min_transfer_tons = 1.0;

  Simulation sim(content, cfg);
  sim.new_game();

  GameState st = sim.state();
  N4X_ASSERT(!st.factions.empty(), "new_game should create a faction");
  const Faction f = st.factions.begin()->second;

  // Keep the test small/controlled.
  st.systems.clear();
  st.bodies.clear();
  st.colonies.clear();
  st.ships.clear();
  st.ship_orders.clear();
  st.fleets.clear();

  // One system.
  StarSystem sys;
  sys.id = 1;
  sys.name = "Sol";
  sys.galaxy_pos = Vec2{0.0, 0.0};
  st.systems[sys.id] = sys;

  // One idle auto-tanker ship with lots of fuel.
  Ship tk;
  tk.id = 100;
  tk.name = "Tanker-1";
  tk.faction_id = f.id;
  tk.design_id = tanker.id;
  tk.system_id = sys.id;
  tk.position_mkm = Vec2{0.0, 0.0};
  tk.auto_tanker = true;
  tk.auto_tanker_reserve_fraction = 0.20;
  tk.fuel_tons = 900.0;
  st.ships[tk.id] = tk;

  // Two idle ships that need fuel.
  Ship a;
  a.id = 200;
  a.name = "LowFuel-A";
  a.faction_id = f.id;
  a.design_id = target.id;
  a.system_id = sys.id;
  a.position_mkm = Vec2{1.0, 0.0};
  a.fuel_tons = 0.0;
  st.ships[a.id] = a;

  Ship b;
  b.id = 201;
  b.name = "LowFuel-B";
  b.faction_id = f.id;
  b.design_id = target.id;
  b.system_id = sys.id;
  b.position_mkm = Vec2{2.0, 0.0};
  b.fuel_tons = 0.0;
  st.ships[b.id] = b;

  sim.load_game(st);

  const GameState base_state = sim.state();

  // --- 1) Planner emits a multi-stop route (2 legs) for the tanker.
  {
    FuelPlannerOptions opt;
    opt.require_auto_tanker_flag = true;
    opt.require_idle = true;
    opt.restrict_to_discovered = false;
    opt.exclude_fleet_ships = true;
    opt.exclude_ships_with_auto_refuel = true;
    opt.max_targets = 100;
    opt.max_tankers = 10;
    opt.max_legs_per_tanker = 4;

    const auto plan = compute_fuel_plan(sim, f.id, opt);
    N4X_ASSERT(plan.ok, "plan ok");
    N4X_ASSERT(!plan.assignments.empty(), "expected at least one tanker assignment");

    const auto& asg = plan.assignments.front();
    N4X_ASSERT(asg.tanker_ship_id == tk.id, "assignment targets tanker");
    N4X_ASSERT(asg.legs.size() == 2, "tanker should be assigned 2 refuel stops");

    // Because both targets are equally low, the closer one (A @ 1 mkm) should come first.
    N4X_ASSERT(asg.legs[0].target_ship_id == a.id, "expected closer target first");
    N4X_ASSERT(asg.legs[1].target_ship_id == b.id, "expected second target next");

    // Each target wants 90% of 200t = 180t.
    N4X_ASSERT(std::abs(asg.legs[0].tons - 180.0) < 1e-6, "leg 1 tons");
    N4X_ASSERT(std::abs(asg.legs[1].tons - 180.0) < 1e-6, "leg 2 tons");

    // Apply and verify orders queued.
    const bool applied = apply_fuel_assignment(sim, asg, /*clear_existing_orders=*/true);
    N4X_ASSERT(applied, "apply_fuel_assignment ok");

    const ShipOrders* so = find_ptr(sim.state().ship_orders, tk.id);
    N4X_ASSERT(so, "tanker has ship_orders");
    N4X_ASSERT(static_cast<int>(so->queue.size()) == 2, "two orders queued (same-system transfers)");
    N4X_ASSERT(count_order_variant<TransferFuelToShip>(*so) == 2, "queued TransferFuelToShip x2");
  }

  // --- 2) Reserved target: if a ship is already targeted by a TransferFuelToShip order, planner skips it.
  {
    // Create a fresh sim with a pre-existing order targeting LowFuel-A.
    Simulation sim2(content, cfg);
    sim2.load_game(base_state);

    // Seed an order from tanker -> A.
    (void)sim2.issue_transfer_fuel_to_ship(tk.id, a.id, 10.0, /*restrict_to_discovered=*/false);

    FuelPlannerOptions opt;
    opt.require_auto_tanker_flag = true;
    opt.require_idle = false;              // tanker isn't idle now
    opt.restrict_to_discovered = false;
    opt.exclude_fleet_ships = true;
    opt.exclude_ships_with_auto_refuel = true;

    const auto plan = compute_fuel_plan(sim2, f.id, opt);
    N4X_ASSERT(plan.ok, "plan ok (reserved)");

    // LowFuel-A is reserved, so only LowFuel-B should be considered.
    bool has_a = false;
    bool has_b = false;
    for (const auto& asg : plan.assignments) {
      for (const auto& leg : asg.legs) {
        if (leg.target_ship_id == a.id) has_a = true;
        if (leg.target_ship_id == b.id) has_b = true;
      }
    }
    N4X_ASSERT(!has_a, "reserved target A should not appear");
    N4X_ASSERT(has_b || plan.assignments.empty(), "either B is planned or no tanker eligible");
  }

  return 0;
}
