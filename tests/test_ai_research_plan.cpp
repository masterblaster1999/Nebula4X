#include <iostream>
#include <string>
#include <vector>

#include "nebula4x/core/ai_economy.h"
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

int test_ai_research_plan() {
  using namespace nebula4x;

  // Load full content so the test matches real campaigns.
  auto content_db = load_content_db_from_file("data/blueprints/starting_blueprints.json");
  content_db.techs = load_tech_db_from_file("data/tech/tech_tree.json");

  SimConfig cfg;
  cfg.enable_combat = false;

  Simulation sim(content_db, cfg);

  // Find an AI pirate faction.
  Id pirate_fid = kInvalidId;
  for (const auto& [fid, f] : sim.state().factions) {
    if (f.control == FactionControl::AI_Pirate) {
      pirate_fid = fid;
      break;
    }
  }
  N4X_ASSERT(pirate_fid != kInvalidId, "Expected an AI_Pirate faction to exist");

  // Force a pathological queue (missing prereqs) and ensure the AI planner repairs it.
  auto& pf = sim.state().factions.at(pirate_fid);
  pf.known_techs.clear();
  pf.active_research_id.clear();
  pf.active_research_progress = 0.0;
  pf.research_queue.clear();
  pf.research_queue.push_back("automation_1");

  tick_ai_economy(sim);

  const auto& q = pf.research_queue;

  auto idx = [&](const std::string& id) -> int {
    for (int i = 0; i < static_cast<int>(q.size()); ++i) {
      if (q[i] == id) return i;
    }
    return -1;
  };

  const int i_chem = idx("chemistry_1");
  const int i_nuc = idx("nuclear_1");
  const int i_reac = idx("reactors_2");
  const int i_mat = idx("materials_processing_1");
  const int i_auto = idx("automation_1");

  N4X_ASSERT(i_chem >= 0, "Expected chemistry_1 to be planned as a prerequisite");
  N4X_ASSERT(i_nuc >= 0, "Expected nuclear_1 to be planned as a prerequisite");
  N4X_ASSERT(i_reac >= 0, "Expected reactors_2 to be planned as a prerequisite");
  N4X_ASSERT(i_mat >= 0, "Expected materials_processing_1 to be planned as a prerequisite");
  N4X_ASSERT(i_auto >= 0, "Expected automation_1 to remain planned");

  // automation_1 prereqs: materials_processing_1 + reactors_2
  // reactors_2 prereq: nuclear_1
  // nuclear_1 prereq: chemistry_1
  N4X_ASSERT(i_chem < i_nuc, "Expected chemistry_1 to precede nuclear_1");
  N4X_ASSERT(i_nuc < i_reac, "Expected nuclear_1 to precede reactors_2");
  N4X_ASSERT(i_chem < i_mat, "Expected chemistry_1 to precede materials_processing_1");
  N4X_ASSERT(i_reac < i_auto, "Expected reactors_2 to precede automation_1");
  N4X_ASSERT(i_mat < i_auto, "Expected materials_processing_1 to precede automation_1");

  return 0;
}
