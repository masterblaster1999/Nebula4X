#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "nebula4x/util/duel_simulator.h"

namespace nebula4x {

class Simulation;

// Options for running a round-robin combat tournament between multiple ship designs.
//
// The tournament is built on top of the existing Duel simulator. Each "task" is
// a duel between a pair of designs, optionally run twice (two_way) with sides swapped
// to reduce any spawn-side bias.
struct DuelRoundRobinOptions {
  // Duel configuration applied to each task.
  //
  // Notes:
  // - duel.runs is interpreted as the number of runs per task (per direction).
  // - duel.seed is used as a base seed; each task derives its own deterministic seed.
  DuelOptions duel;

  // Spawn count per design per run (symmetric).
  int count_per_side{1};

  // If true (default), each pair (i,j) is executed twice: i-vs-j and j-vs-i.
  bool two_way{true};

  // Elo ratings (optional).
  bool compute_elo{true};
  double elo_initial{1000.0};
  double elo_k_factor{32.0};
};

// Aggregate tournament result.
//
// Matrices are NxN where N = design_ids.size().
// - games[a][b] counts total games between a and b (mirrored).
// - wins[a][b] counts how many times a defeated b.
// - draws[a][b] counts draws (mirrored).
// - sum_days[a][b] is the sum of days simulated for those games (mirrored).
struct DuelRoundRobinResult {
  std::vector<std::string> design_ids;
  DuelRoundRobinOptions options;

  std::vector<double> elo;
  std::vector<int> total_wins;
  std::vector<int> total_losses;
  std::vector<int> total_draws;

  std::vector<std::vector<int>> games;
  std::vector<std::vector<int>> wins;
  std::vector<std::vector<int>> draws;
  std::vector<std::vector<double>> sum_days;
};

// Incremental tournament runner designed for UI use.
//
// The runner mutates the supplied Simulation (as a sandbox) by repeatedly loading
// duel states. Do not point this at the player's live Simulation state.
class DuelRoundRobinRunner {
 public:
  DuelRoundRobinRunner(Simulation& sim, std::vector<std::string> design_ids, DuelRoundRobinOptions options);

  bool ok() const { return ok_; }
  const std::string& error() const { return error_; }

  bool done() const { return done_; }

  int total_tasks() const { return total_tasks_; }
  int completed_tasks() const { return completed_tasks_; }
  double progress() const;

  // Human-readable label for the next task.
  std::string current_task_label() const;

  // Executes up to max_tasks duel tasks.
  //
  // A "task" is one duel direction (i-vs-j). When options.two_way is enabled,
  // each unordered pair produces two tasks.
  void step(int max_tasks = 1);

  const DuelRoundRobinResult& result() const { return result_; }

 private:
  Simulation& sim_;
  DuelRoundRobinOptions options_;
  DuelRoundRobinResult result_;

  bool ok_{true};
  bool done_{false};
  std::string error_;

  int n_{0};
  int i_{0};
  int j_{1};
  int dir_{0};
  int total_tasks_{0};
  int completed_tasks_{0};
};

// Convenience helper that runs the entire tournament to completion.
//
// On failure, ok=false and error (if provided) is filled.
DuelRoundRobinResult run_duel_round_robin(Simulation& sim,
                                         const std::vector<std::string>& design_ids,
                                         DuelRoundRobinOptions options,
                                         std::string* error = nullptr);

// Serialize the tournament result to JSON text.
std::string duel_round_robin_to_json(const DuelRoundRobinResult& result, int indent = 2);

}  // namespace nebula4x
