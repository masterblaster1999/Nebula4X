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

  return 0;
}
