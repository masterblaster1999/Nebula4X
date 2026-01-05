#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace nebula4x {

class Simulation;

// High-level spec for one side of a duel.
struct DuelSideSpec {
  // Ship design id from ContentDB (or a custom design loaded into Simulation).
  std::string design_id;

  // Number of ships to spawn for this side.
  int count{1};

  // Optional human-readable label ("A"/"B", "Red"/"Blue", etc).
  std::string label;
};

struct DuelOptions {
  // Maximum number of simulation days to run per duel.
  int max_days{200};

  // Initial separation between the two forces (million km).
  //
  // If <= 0, the simulator will choose a heuristic distance based on weapon/missile ranges.
  double initial_separation_mkm{-1.0};

  // Random +/- position jitter applied per-ship (million km).
  double position_jitter_mkm{0.0};

  // Number of independent runs to execute.
  int runs{1};

  // Base RNG seed used for position jitter and per-run variation.
  std::uint32_t seed{1};

  // When true (default), the simulator issues AttackShip orders so forces
  // will close into range even if they spawn outside weapon range.
  bool issue_attack_orders{true};

  // When true, include a final state digest per-run in the summary.
  bool include_final_state_digest{true};
};

struct DuelRunResult {
  int run_index{0};
  std::uint32_t seed{1};

  // Days actually simulated (<= options.max_days).
  int days_simulated{0};

  // "A", "B", or "Draw".
  std::string winner;

  int a_survivors{0};
  int b_survivors{0};

  double a_total_hp{0.0};
  double b_total_hp{0.0};

  // Hex string (e.g. "0x0123...") when include_final_state_digest=true, else empty.
  std::string final_state_digest_hex;
};

struct DuelAggregateResult {
  DuelSideSpec a;
  DuelSideSpec b;
  DuelOptions options;

  std::vector<DuelRunResult> runs;

  int a_wins{0};
  int b_wins{0};
  int draws{0};

  double a_win_rate{0.0};
  double b_win_rate{0.0};
  double draw_rate{0.0};

  double avg_days{0.0};
  double avg_a_survivors{0.0};
  double avg_b_survivors{0.0};
};

// Runs a design-vs-design combat duel using the existing simulation rules.
// The provided Simulation instance is used as a sandbox and its state will be
// overwritten during execution.
//
// On failure, ok=false and error (if provided) is filled.
DuelAggregateResult run_design_duel(Simulation& sim,
                                   DuelSideSpec a,
                                   DuelSideSpec b,
                                   DuelOptions options,
                                   std::string* error = nullptr);

// Serialize a duel result to JSON text.
//
// Notes:
// - 64-bit digests are emitted as hex strings to avoid JSON number precision loss.
// - The JSON schema is intended for tooling / balance regression tests.
std::string duel_result_to_json(const DuelAggregateResult& result, int indent = 2);

}  // namespace nebula4x
