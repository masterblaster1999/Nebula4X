#include "nebula4x/util/duel_tournament.h"

#include "nebula4x/core/simulation.h"

#include "tests/test.h"

#include <iostream>

namespace {

#define N4X_ASSERT(cond)                                                                          \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      std::cerr << "ASSERT FAILED: " << #cond << " at " << __FILE__ << ":" << __LINE__ << "\n"; \
      return 1;                                                                                    \
    }                                                                                              \
  } while (0)

} // namespace

int test_duel_tournament() {
  nebula4x::ContentDB content;

  // Three designs: strong > medium > weak.
  nebula4x::ShipDesign strong;
  strong.id = "strong";
  strong.name = "Strong";
  strong.role = nebula4x::ShipRole::Combatant;
  strong.max_hp = 100.0;
  strong.speed_km_s = 0.0;
  strong.weapon_damage = 20.0;
  strong.weapon_range_mkm = 1.0;
  content.designs[strong.id] = strong;

  nebula4x::ShipDesign medium;
  medium.id = "medium";
  medium.name = "Medium";
  medium.role = nebula4x::ShipRole::Combatant;
  medium.max_hp = 100.0;
  medium.speed_km_s = 0.0;
  medium.weapon_damage = 5.0;
  medium.weapon_range_mkm = 1.0;
  content.designs[medium.id] = medium;

  nebula4x::ShipDesign weak;
  weak.id = "weak";
  weak.name = "Weak";
  weak.role = nebula4x::ShipRole::Combatant;
  weak.max_hp = 100.0;
  weak.speed_km_s = 0.0;
  weak.weapon_damage = 1.0;
  weak.weapon_range_mkm = 1.0;
  content.designs[weak.id] = weak;

  nebula4x::Simulation sim(std::move(content), nebula4x::SimConfig{});

  nebula4x::DuelRoundRobinOptions opt;
  opt.count_per_side = 1;
  opt.two_way = true;
  opt.compute_elo = true;
  opt.elo_initial = 1000.0;
  opt.elo_k_factor = 32.0;

  opt.duel.max_days = 20;
  opt.duel.initial_separation_mkm = 0.5; // within weapon range
  opt.duel.position_jitter_mkm = 0.0;
  opt.duel.runs = 1;
  opt.duel.seed = 123;
  opt.duel.issue_attack_orders = false;
  opt.duel.include_final_state_digest = false;

  const std::vector<std::string> roster = {"strong", "medium", "weak"};

  std::string err;
  const auto res = nebula4x::run_duel_round_robin(sim, roster, opt, &err);
  N4X_ASSERT(err.empty());
  N4X_ASSERT(res.design_ids.size() == 3);

  // Each unordered pair is executed twice (two_way) with 1 run per task.
  // So games between any two should be 2.
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) {
      if (i == j) {
        N4X_ASSERT(res.games[i][j] == 0);
      } else {
        N4X_ASSERT(res.games[i][j] == 2);
      }
    }
  }

  // Strong should beat medium and weak.
  const int idx_strong = 0;
  const int idx_medium = 1;
  const int idx_weak = 2;

  N4X_ASSERT(res.wins[idx_strong][idx_medium] == 2);
  N4X_ASSERT(res.wins[idx_strong][idx_weak] == 2);
  // Medium should beat weak.
  N4X_ASSERT(res.wins[idx_medium][idx_weak] == 2);

  // Elo ordering should reflect strength.
  N4X_ASSERT(res.elo[idx_strong] > res.elo[idx_medium]);
  N4X_ASSERT(res.elo[idx_medium] > res.elo[idx_weak]);

  return 0;
}
