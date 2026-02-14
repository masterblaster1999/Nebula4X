#include "nebula4x/util/duel_swiss_tournament.h"

#include "nebula4x/core/simulation.h"

#include "tests/test.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <unordered_set>

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

  // Four designs for a compact deterministic Swiss schedule.
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
  // Keep this test focused on Swiss bookkeeping/determinism rather than combat AI.
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

  // Points accounting should match W/L/D aggregates.
  for (int i = 0; i < 4; ++i) {
    const double from_wld = static_cast<double>(res.total_wins[i]) + 0.5 * static_cast<double>(res.total_draws[i]);
    N4X_ASSERT(std::fabs(res.points[i] - from_wld) < 1e-9);
    N4X_ASSERT(std::isfinite(res.elo[i]));
  }

  // Total Swiss points: one point per game.
  double total_points = 0.0;
  for (double p : res.points) total_points += p;
  const double expected_total_points =
      static_cast<double>(opt.rounds) * static_cast<double>(roster.size() / 2) * static_cast<double>(opt.duel.runs);
  N4X_ASSERT(std::fabs(total_points - expected_total_points) < 1e-9);

  // With 4 players and 3 rounds, pairings should cover each unordered pair once.
  std::unordered_set<std::uint64_t> seen_pairs;
  for (const auto& rr : res.rounds) {
    N4X_ASSERT(static_cast<int>(rr.matches.size()) == 2);
    for (const auto& m : rr.matches) {
      N4X_ASSERT(!m.bye);
      N4X_ASSERT(m.a >= 0 && m.a < 4);
      N4X_ASSERT(m.b >= 0 && m.b < 4);
      N4X_ASSERT(m.a != m.b);
      N4X_ASSERT(m.games == opt.duel.runs * (opt.two_way ? 2 : 1));
      N4X_ASSERT(m.a_wins + m.b_wins + m.draws == m.games);

      const std::uint32_t lo = static_cast<std::uint32_t>(std::min(m.a, m.b));
      const std::uint32_t hi = static_cast<std::uint32_t>(std::max(m.a, m.b));
      const std::uint64_t key = (static_cast<std::uint64_t>(hi) << 32) | static_cast<std::uint64_t>(lo);
      N4X_ASSERT(seen_pairs.insert(key).second);
    }
  }
  N4X_ASSERT(seen_pairs.size() == 6);

  // Determinism: running again with the same seed should yield identical JSON.
  std::string err2;
  const auto res2 = nebula4x::run_duel_swiss(sim, roster, opt, &err2);
  N4X_ASSERT(err2.empty());
  N4X_ASSERT(nebula4x::duel_swiss_to_json(res) == nebula4x::duel_swiss_to_json(res2));

  return 0;
}
