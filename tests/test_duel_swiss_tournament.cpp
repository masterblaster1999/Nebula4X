#include "nebula4x/util/duel_swiss_tournament.h"

#include "nebula4x/core/simulation.h"

#include "tests/test.h"

#include <cmath>
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

int test_duel_swiss_tournament() {
  nebula4x::ContentDB content;

  // Four designs with clear ordering: strong > medium > weak > ultra.
  auto mk = [&](const std::string& id, double dmg) {
    nebula4x::ShipDesign d;
    d.id = id;
    d.name = id;
    d.role = nebula4x::ShipRole::Combatant;
    d.max_hp = 100.0;
    d.speed_km_s = 0.0;
    d.weapon_damage = dmg;
    d.weapon_range_mkm = 1.0;
    content.designs[d.id] = d;
  };

  mk("strong", 20.0);
  mk("medium", 5.0);
  mk("weak", 1.0);
  mk("ultra", 0.2);

  nebula4x::Simulation sim(std::move(content), nebula4x::SimConfig{});

  nebula4x::DuelSwissOptions opt;
  opt.count_per_side = 1;
  opt.rounds = 3;
  opt.two_way = false;  // easier to reason about game counts
  opt.compute_elo = true;
  opt.elo_initial = 1000.0;
  opt.elo_k_factor = 32.0;

  opt.duel.max_days = 20;
  opt.duel.initial_separation_mkm = 0.5;  // within weapon range
  opt.duel.position_jitter_mkm = 0.0;
  opt.duel.runs = 1;
  opt.duel.seed = 123;
  opt.duel.issue_attack_orders = false;
  opt.duel.include_final_state_digest = false;

  const std::vector<std::string> roster = {"strong", "medium", "weak", "ultra"};

  std::string err;
  const auto res = nebula4x::run_duel_swiss(sim, roster, opt, &err);
  N4X_ASSERT(err.empty());
  N4X_ASSERT(res.design_ids.size() == 4);
  N4X_ASSERT(static_cast<int>(res.rounds.size()) == opt.rounds);

  // Even roster: no byes.
  for (int i = 0; i < 4; ++i) {
    N4X_ASSERT(res.byes[i] == 0);
  }

  // Each player should have exactly 'rounds' results (wins+losses+draws).
  for (int i = 0; i < 4; ++i) {
    const int g = res.total_wins[i] + res.total_losses[i] + res.total_draws[i];
    N4X_ASSERT(g == opt.rounds);
  }

  const int idx_strong = 0;
  const int idx_medium = 1;
  const int idx_weak = 2;
  const int idx_ultra = 3;

  // With 4 designs and 3 rounds, the greedy Swiss pairing should cover all
  // unique pairings once (i.e. equivalent to full round robin).
  // That yields deterministic point totals for our strictly-ordered designs:
  // strong=3, medium=2, weak=1, ultra=0.
  N4X_ASSERT(std::fabs(res.points[idx_strong] - 3.0) < 1e-9);
  N4X_ASSERT(std::fabs(res.points[idx_medium] - 2.0) < 1e-9);
  N4X_ASSERT(std::fabs(res.points[idx_weak] - 1.0) < 1e-9);
  N4X_ASSERT(std::fabs(res.points[idx_ultra] - 0.0) < 1e-9);

  // Elo should preserve ordering.
  N4X_ASSERT(res.elo[idx_strong] > res.elo[idx_medium]);
  N4X_ASSERT(res.elo[idx_medium] > res.elo[idx_weak]);
  N4X_ASSERT(res.elo[idx_weak] > res.elo[idx_ultra]);

  // Determinism: running again with the same seed should yield identical JSON.
  std::string err2;
  const auto res2 = nebula4x::run_duel_swiss(sim, roster, opt, &err2);
  N4X_ASSERT(err2.empty());
  N4X_ASSERT(nebula4x::duel_swiss_to_json(res) == nebula4x::duel_swiss_to_json(res2));

  return 0;
}
