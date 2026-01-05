#include "nebula4x/util/duel_simulator.h"

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

int test_duel_simulator() {
  nebula4x::ContentDB content;

  // "Strong" wins a 1v1 duel within a handful of days.
  nebula4x::ShipDesign strong;
  strong.id = "strong";
  strong.name = "Strong";
  strong.role = nebula4x::ShipRole::Combatant;
  strong.max_hp = 100.0;
  strong.speed_km_s = 0.0;
  strong.weapon_damage = 20.0;
  strong.weapon_range_mkm = 1.0;
  content.designs[strong.id] = strong;

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

  nebula4x::DuelSideSpec a;
  a.design_id = "strong";
  a.count = 1;
  a.label = "A";

  nebula4x::DuelSideSpec b;
  b.design_id = "weak";
  b.count = 1;
  b.label = "B";

  nebula4x::DuelOptions opt;
  opt.max_days = 20;
  opt.initial_separation_mkm = 0.5; // within weapon range
  opt.position_jitter_mkm = 0.0;
  opt.runs = 1;
  opt.seed = 123;
  opt.issue_attack_orders = false; // no movement needed for this test

  std::string err;
  const auto res = nebula4x::run_design_duel(sim, a, b, opt, &err);
  N4X_ASSERT(err.empty());
  N4X_ASSERT(res.runs.size() == 1);

  const auto& r0 = res.runs[0];
  N4X_ASSERT(r0.winner == "A");
  N4X_ASSERT(r0.a_survivors == 1);
  N4X_ASSERT(r0.b_survivors == 0);
  N4X_ASSERT(r0.days_simulated > 0 && r0.days_simulated <= opt.max_days);

  N4X_ASSERT(res.a_wins == 1);
  N4X_ASSERT(res.b_wins == 0);
  N4X_ASSERT(res.draws == 0);

  return 0;
}
