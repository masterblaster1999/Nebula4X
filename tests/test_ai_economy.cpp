#include <iostream>
#include <string>

#include "nebula4x/core/simulation.h"
#include "nebula4x/core/tech.h"

#define N4X_ASSERT(cond, msg)                                                       \
  do {                                                                              \
    if (!(cond)) {                                                                  \
      std::cerr << "ASSERT FAILED: " << (msg) << "\n  at " << __FILE__ << ":"       \
                << __LINE__ << "\n";                                                \
      return 1;                                                                     \
    }                                                                               \
  } while (0)

int test_ai_economy() {
  using namespace nebula4x;

  // Load full content so we exercise the real blueprint/tech DB.
  auto content_db = load_content_db_from_file("data/blueprints/starting_blueprints.json");
  content_db.techs = load_tech_db_from_file("data/tech/tech_tree.json");

  SimConfig cfg;
  cfg.enable_combat = false;  // keep the test stable (no ships getting destroyed)

  Simulation sim(content_db, cfg);

  // Find the pirate faction by control mode.
  Id pirate_fid = kInvalidId;
  for (const auto& [fid, f] : sim.state().factions) {
    if (f.control == FactionControl::AI_Pirate) {
      pirate_fid = fid;
      break;
    }
  }
  N4X_ASSERT(pirate_fid != kInvalidId, "Expected an AI_Pirate faction to exist");

  int pirate_colonies = 0;
  for (const auto& [cid, c] : sim.state().colonies) {
    if (c.faction_id == pirate_fid) pirate_colonies++;
  }
  N4X_ASSERT(pirate_colonies >= 1, "Expected pirates to start with a base colony");

  auto count_pirate_ships = [&]() -> int {
    int n = 0;
    for (const auto& [sid, s] : sim.state().ships) {
      if (s.faction_id == pirate_fid) n++;
    }
    return n;
  };

  const int initial_ships = count_pirate_ships();

  // Let the AI plan + shipyard build a bit.
  sim.advance_days(60);

  const int after_60 = count_pirate_ships();
  N4X_ASSERT(after_60 > initial_ships, "Expected pirates to have built at least one ship");

  // Give enough time to progress through chemistry->nuclear->propulsion and start
  // building upgraded hulls.
  sim.advance_days(220);

  bool built_upgrade = false;
  for (const auto& [sid, s] : sim.state().ships) {
    if (s.faction_id != pirate_fid) continue;
    if (s.design_id == "pirate_raider_ion" || s.design_id == "pirate_raider_mk2") {
      built_upgrade = true;
      break;
    }
  }
  N4X_ASSERT(built_upgrade, "Expected pirates to eventually build an upgraded raider design");

  return 0;
}

