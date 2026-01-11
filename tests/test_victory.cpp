#include <iostream>

#include "nebula4x/core/scenario.h"
#include "nebula4x/core/simulation.h"
#include "nebula4x/core/tech.h"

#define N4X_ASSERT(cond, msg)                                                        \
  do {                                                                               \
    if (!(cond)) {                                                                   \
      std::cerr << "Assertion failed: " << (msg) << " (" << __FILE__ << ":"      \
                << __LINE__ << ")\n";                                             \
      return 1;                                                                      \
    }                                                                                \
  } while (0)

namespace nebula4x {

int test_victory() {
  ContentDB content = load_content_db_from_file("data/blueprints/starting_blueprints.json");

  SimConfig cfg;
  cfg.enable_combat = false;

  Simulation sim(content, cfg);

  RandomScenarioConfig sc;
  sc.seed = 424242u;
  sc.num_systems = 8;
  sc.enable_pirates = false;
  sc.num_ai_empires = 1;
  sc.enable_regions = false;

  GameState st = make_random_scenario(sc);
  sim.load_game(st);

  // --- Score invariants ---
  const auto scores0 = sim.compute_scoreboard();
  N4X_ASSERT(!scores0.empty(), "Expected non-empty scoreboard");
  N4X_ASSERT(scores0.size() == sim.state().factions.size(), "Scoreboard size mismatch");

  for (size_t i = 1; i < scores0.size(); ++i) {
    const double prev = scores0[i - 1].score.total_points();
    const double cur = scores0[i].score.total_points();
    N4X_ASSERT(prev + 1e-6 >= cur, "Scoreboard not sorted descending");
  }

  // If we increase a faction's population, their score should increase.
  const Id fid = scores0.front().faction_id;

  bool bumped = false;
  for (auto& kv : sim.state().colonies) {
    Colony& c = kv.second;
    if (c.faction_id == fid) {
      c.population_millions *= 2.0;
      bumped = true;
      break;
    }
  }
  N4X_ASSERT(bumped, "Unable to find a colony to adjust population for score test");

  const auto scores1 = sim.compute_scoreboard();
  double before = 0.0;
  double after = 0.0;
  for (const auto& e : scores0) {
    if (e.faction_id == fid) before = e.score.total_points();
  }
  for (const auto& e : scores1) {
    if (e.faction_id == fid) after = e.score.total_points();
  }
  N4X_ASSERT(after > before, "Expected increasing population to increase score");

  // --- Victory trigger (elimination) ---
  sim.state().victory_rules.enabled = true;
  sim.state().victory_rules.exclude_pirates = true;
  sim.state().victory_rules.elimination_enabled = true;
  sim.state().victory_rules.elimination_requires_colony = true;
  sim.state().victory_rules.score_threshold = 0.0;

  // Identify player + AI factions.
  Id player = kInvalidId;
  Id ai = kInvalidId;
  for (const auto& kv : sim.state().factions) {
    if (kv.second.control == FactionControl::Player && player == kInvalidId) {
      player = kv.first;
    }
    if (kv.second.control == FactionControl::AI_Explorer && ai == kInvalidId) {
      ai = kv.first;
    }
  }
  N4X_ASSERT(player != kInvalidId, "Expected player faction");
  N4X_ASSERT(ai != kInvalidId, "Expected AI explorer faction");

  // Transfer all AI colonies to the player so the AI is no longer "alive".
  for (auto& kv : sim.state().colonies) {
    if (kv.second.faction_id == ai) {
      kv.second.faction_id = player;
    }
  }

  sim.advance_days(1);

  N4X_ASSERT(sim.state().victory_state.game_over, "Expected victory to be declared");
  N4X_ASSERT(sim.state().victory_state.winner_faction_id == player,
             "Expected player to win elimination victory");
  N4X_ASSERT(sim.state().victory_state.reason == VictoryReason::LastFactionStanding,
             "Expected elimination victory reason");

  return 0;
}

} // namespace nebula4x
