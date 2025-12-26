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
  return 0;
}
