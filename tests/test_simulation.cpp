#include <iostream>

#include "nebula4x/core/simulation.h"

#define N4X_ASSERT(expr) \
  do { \
    if (!(expr)) { \
      std::cerr << "ASSERT failed: " #expr " (" << __FILE__ << ":" << __LINE__ << ")\n"; \
      return 1; \
    } \
  } while (0)

int test_simulation() {
  // --- mineral production sanity check ---
  // Minimal content DB for installations.
  nebula4x::ContentDB content;
  nebula4x::InstallationDef mine;
  mine.id = "automated_mine";
  mine.name = "Automated Mine";
  mine.produces_per_day = {{"Duranium", 1.0}};
  content.installations[mine.id] = mine;

  nebula4x::InstallationDef yard;
  yard.id = "shipyard";
  yard.name = "Shipyard";
  yard.build_rate_tons_per_day = 50;
  content.installations[yard.id] = yard;

  // Minimal design.
  nebula4x::ShipDesign d;
  d.id = "freighter_alpha";
  d.name = "Freighter Alpha";
  d.mass_tons = 100;
  d.speed_km_s = 10;
  content.designs[d.id] = d;

  nebula4x::Simulation sim(std::move(content), nebula4x::SimConfig{});

  auto before = sim.state().colonies.begin()->second.minerals["Duranium"];
  sim.advance_days(2);
  auto after = sim.state().colonies.begin()->second.minerals["Duranium"];

  N4X_ASSERT(after > before);

  // --- shipyard build costs sanity check ---
  // When build_costs_per_ton are configured for the shipyard, advancing time with an
  // active build order should consume minerals.
  nebula4x::ContentDB content2;

  nebula4x::InstallationDef yard2;
  yard2.id = "shipyard";
  yard2.name = "Shipyard";
  yard2.build_rate_tons_per_day = 50;
  yard2.build_costs_per_ton = {{"Duranium", 1.0}}; // 1 mineral per ton
  content2.installations[yard2.id] = yard2;

  // No mineral production to keep the check simple.
  nebula4x::InstallationDef mine2;
  mine2.id = "automated_mine";
  mine2.name = "Automated Mine";
  mine2.produces_per_day = {{"Duranium", 0.0}};
  content2.installations[mine2.id] = mine2;

  // Minimal design.
  nebula4x::ShipDesign d2;
  d2.id = "freighter_alpha";
  d2.name = "Freighter Alpha";
  d2.mass_tons = 100;
  d2.speed_km_s = 10;
  content2.designs[d2.id] = d2;

  nebula4x::Simulation sim2(std::move(content2), nebula4x::SimConfig{});

  const auto colony_id = sim2.state().colonies.begin()->first;
  const double before_build = sim2.state().colonies.begin()->second.minerals["Duranium"];
  N4X_ASSERT(sim2.enqueue_build(colony_id, "freighter_alpha"));
  sim2.advance_days(1);
  const double after_build = sim2.state().colonies.begin()->second.minerals["Duranium"];
  N4X_ASSERT(after_build < before_build);

  // --- colony construction queue sanity check ---
  // Enqueuing an installation build should consume minerals and increase
  // installation count once enough construction points are available.
  nebula4x::ContentDB content3;

  nebula4x::InstallationDef mine3;
  mine3.id = "automated_mine";
  mine3.name = "Automated Mine";
  mine3.produces_per_day = {{"Duranium", 0.0}}; // avoid production influencing the check
  mine3.construction_cost = 50.0;
  mine3.build_costs = {{"Duranium", 100.0}};
  content3.installations[mine3.id] = mine3;

  nebula4x::InstallationDef yard3;
  yard3.id = "shipyard";
  yard3.name = "Shipyard";
  yard3.build_rate_tons_per_day = 0.0;
  content3.installations[yard3.id] = yard3;

  // Keep designs minimal; none are needed for this test.
  nebula4x::Simulation sim3(std::move(content3), nebula4x::SimConfig{});
  const auto colony3_id = sim3.state().colonies.begin()->first;
  auto& col3 = sim3.state().colonies.begin()->second;

  const int mines_before = col3.installations["automated_mine"];
  const double dur_before = col3.minerals["Duranium"];

  N4X_ASSERT(sim3.enqueue_installation_build(colony3_id, "automated_mine", 1));
  sim3.advance_days(1);

  const int mines_after = sim3.state().colonies.begin()->second.installations["automated_mine"];
  const double dur_after = sim3.state().colonies.begin()->second.minerals["Duranium"];

  N4X_ASSERT(mines_after == mines_before + 1);
  N4X_ASSERT(dur_after < dur_before);

  return 0;
}
