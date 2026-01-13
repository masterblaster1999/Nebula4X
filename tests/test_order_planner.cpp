#include <cmath>
#include <iostream>
#include <string>

#include "nebula4x/core/order_planner.h"
#include "nebula4x/core/simulation.h"

#define N4X_ASSERT(expr)                                                                            \
  do {                                                                                              \
    if (!(expr)) {                                                                                  \
      std::cerr << "ASSERT failed: " #expr " (" << __FILE__ << ":" << __LINE__ << ")\n";            \
      return 1;                                                                                     \
    }                                                                                               \
  } while (0)

namespace {

bool approx(double a, double b, double eps = 1e-6) { return std::fabs(a - b) <= eps; }

} // namespace

int test_order_planner() {
  using namespace nebula4x;

  // Minimal content: one ship design with speed + fuel stats.
  ContentDB content;
  {
    ShipDesign d;
    d.id = "test_design";
    d.name = "Test Design";
    d.speed_km_s = 1000.0;
    d.fuel_capacity_tons = 500.0;
    d.fuel_use_per_mkm = 1.0;
    d.weapon_range_mkm = 50.0;
    d.sensor_range_mkm = 100.0;
    content.designs[d.id] = d;
  }

  SimConfig cfg;
  cfg.seconds_per_day = 86400.0;
  cfg.docking_range_mkm = 3.0;
  cfg.arrival_epsilon_mkm = 1e-6;

  Simulation sim(content, cfg);
  auto& st = sim.state();
  st.date = Date(0);
  st.hour_of_day = 0;

  // System + body + colony for MoveToBody ETA/fuel test.
  {
    StarSystem sys;
    sys.id = 1;
    sys.name = "Sys1";
    st.systems[sys.id] = sys;

    Body b;
    b.id = 10;
    b.name = "Earth";
    b.system_id = sys.id;
    b.parent_body_id = kInvalidId;
    b.position_mkm = {100.0, 0.0};
    st.bodies[b.id] = b;

    Colony c;
    c.id = 100;
    c.name = "Earth Colony";
    c.body_id = b.id;
    c.faction_id = 1;
    c.minerals["Fuel"] = 1000.0;
    st.colonies[c.id] = c;

    Ship sh;
    sh.id = 200;
    sh.name = "Test Ship";
    sh.faction_id = 1;
    sh.system_id = sys.id;
    sh.design_id = "test_design";
    sh.position_mkm = {0.0, 0.0};
    sh.speed_km_s = 1000.0;
    sh.fuel_tons = 100.0;
    sh.hp = 100.0;
    st.ships[sh.id] = sh;

    ShipOrders so;
    so.queue.push_back(MoveToBody{b.id});
    st.ship_orders[sh.id] = so;

    OrderPlannerOptions opts;
    opts.predict_orbits = false;   // use cached positions
    opts.simulate_refuel = false;  // keep deterministic

    const OrderPlan plan = compute_order_plan(sim, sh.id, opts);
    N4X_ASSERT(plan.ok);
    N4X_ASSERT(plan.steps.size() == 1);

    const PlannedOrderStep& step = plan.steps[0];

    // Distance to cover is (100 - docking_range).
    const double cover_mkm = 97.0;
    const double mkm_per_day = (1000.0 * 86400.0) / 1.0e6;
    const double expected_dt = cover_mkm / mkm_per_day;

    N4X_ASSERT(approx(step.delta_days, expected_dt, 1e-6));
    N4X_ASSERT(approx(step.eta_days, expected_dt, 1e-6));
    N4X_ASSERT(approx(step.fuel_after_tons, 3.0, 1e-6));
  }

  // Jump transit: TravelViaJump should update system + position (no time for transit itself).
  {
    // Create a second system.
    StarSystem sys2;
    sys2.id = 2;
    sys2.name = "Sys2";
    st.systems[sys2.id] = sys2;

    JumpPoint jp_a;
    jp_a.id = 50;
    jp_a.name = "JP A";
    jp_a.system_id = 1;
    jp_a.position_mkm = {0.0, 0.0};
    jp_a.linked_jump_id = 51;
    st.jump_points[jp_a.id] = jp_a;

    JumpPoint jp_b;
    jp_b.id = 51;
    jp_b.name = "JP B";
    jp_b.system_id = 2;
    jp_b.position_mkm = {10.0, 0.0};
    jp_b.linked_jump_id = 50;
    st.jump_points[jp_b.id] = jp_b;

    Ship sh;
    sh.id = 201;
    sh.name = "Jumper";
    sh.faction_id = 1;
    sh.system_id = 1;
    sh.design_id = "test_design";
    sh.position_mkm = {10.0, 0.0};
    sh.speed_km_s = 1000.0;
    sh.fuel_tons = 500.0;
    sh.hp = 100.0;
    st.ships[sh.id] = sh;

    ShipOrders so;
    so.queue.push_back(TravelViaJump{jp_a.id});
    st.ship_orders[sh.id] = so;

    OrderPlannerOptions opts;
    opts.predict_orbits = false;
    opts.simulate_refuel = false;

    const OrderPlan plan = compute_order_plan(sim, sh.id, opts);
    N4X_ASSERT(plan.ok);
    N4X_ASSERT(plan.steps.size() == 1);
    N4X_ASSERT(plan.steps[0].system_id == 2);
    N4X_ASSERT(approx(plan.steps[0].position_mkm.x, 10.0, 1e-9));
    N4X_ASSERT(approx(plan.steps[0].position_mkm.y, 0.0, 1e-9));
  }

  // Investigate anomaly: planner should include both travel-to-investigation-range and investigation time.
  {
    // Create an anomaly in Sys1.
    Anomaly an;
    an.id = 300;
    an.name = "Test Anomaly";
    an.kind = "test";
    an.system_id = 1;
    an.position_mkm = {200.0, 0.0};
    an.investigation_days = 5;
    an.resolved = false;
    st.anomalies[an.id] = an;

    Ship sh;
    sh.id = 202;
    sh.name = "Investigator";
    sh.faction_id = 1;
    sh.system_id = 1;
    sh.design_id = "test_design";
    sh.position_mkm = {0.0, 0.0};
    sh.speed_km_s = 1000.0;
    sh.fuel_tons = 500.0;
    sh.hp = 100.0;
    st.ships[sh.id] = sh;

    ShipOrders so;
    // duration_days == 0: use anomaly default.
    so.queue.push_back(InvestigateAnomaly{an.id, 0, 0.0});
    st.ship_orders[sh.id] = so;

    OrderPlannerOptions opts;
    opts.predict_orbits = false;
    opts.simulate_refuel = false;

    const OrderPlan plan = compute_order_plan(sim, sh.id, opts);
    N4X_ASSERT(plan.ok);
    N4X_ASSERT(plan.steps.size() == 1);

    const PlannedOrderStep& step = plan.steps[0];

    // Need to get within max(dock_range, sensor_range*0.5) = max(3, 50) = 50.
    // Dist is 200 => cover 150.
    const double cover_mkm = 150.0;
    const double mkm_per_day = (1000.0 * 86400.0) / 1.0e6;
    const double expected_travel_dt = cover_mkm / mkm_per_day;
    const double expected_total_dt = expected_travel_dt + 5.0;

    N4X_ASSERT(approx(step.delta_days, expected_total_dt, 1e-6));
    N4X_ASSERT(approx(step.eta_days, expected_total_dt, 1e-6));
    N4X_ASSERT(approx(step.fuel_after_tons, 350.0, 1e-6));
  }

  return 0;
}
