#include <iostream>

#include "nebula4x/core/scenario.h"
#include "nebula4x/core/simulation.h"
#include "nebula4x/core/tech.h"

#define N4X_ASSERT(cond, msg)                                                        \
  do {                                                                               \
    if (!(cond)) {                                                                   \
      std::cerr << "Assertion failed: " << (msg) << "\n";                         \
      return 1;                                                                      \
    }                                                                                \
  } while (0)

namespace nebula4x {

int test_ai_empire_fleet_missions() {
  // Build a small random scenario with one AI empire and no pirates so we can
  // validate the "AI forms fleets + assigns missions" layer deterministically.
  ContentDB content = load_content_db_from_file("data/blueprints/starting_blueprints.json");
  SimConfig cfg;
  cfg.enable_combat = false;

  Simulation sim(content, cfg);

  RandomScenarioConfig sc;
  sc.seed = 1337;
  sc.num_systems = 12;
  sc.enable_pirates = false;
  sc.num_ai_empires = 1;

  GameState st = make_random_scenario(sc);
  sim.load_game(st);

  // Find the AI empire faction.
  Id ai_faction = kInvalidId;
  for (const auto& kv : sim.state().factions) {
    if (kv.second.control == FactionControl::AI_Explorer) {
      ai_faction = kv.first;
      break;
    }
  }
  N4X_ASSERT(ai_faction != kInvalidId, "Expected at least one AI_Explorer faction");

  // Advance a few days so the AI fleet organizer and mission runner can act.
  sim.advance_days(3);

  // Validate that at least one fleet with a mission exists for this faction.
  bool found_mission_fleet = false;
  for (const auto& kv : sim.state().fleets) {
    const Fleet& fl = kv.second;
    if (fl.faction_id != ai_faction) continue;
    if (fl.mission.type == FleetMissionType::None) continue;
    found_mission_fleet = true;

    // Validate that at least one ship in the fleet has non-empty orders.
    bool any_orders = false;
    for (Id sid : fl.ship_ids) {
      auto it = sim.state().ship_orders.find(sid);
      if (it == sim.state().ship_orders.end()) continue;
      if (!it->second.queue.empty()) {
        any_orders = true;
        break;
      }
    }
    N4X_ASSERT(any_orders, "Mission fleet should be issuing orders to its ships");
  }

  N4X_ASSERT(found_mission_fleet, "AI empire should have at least one mission-enabled fleet");
  return 0;
}

}  // namespace nebula4x
