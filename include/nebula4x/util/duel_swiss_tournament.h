#pragma once

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

#include "nebula4x/util/duel_simulator.h"

namespace nebula4x {

class Simulation;

// Options for running a Swiss-system combat tournament between multiple ship designs.
//
// Compared to round-robin (O(N^2) matchups), Swiss scales much better for large rosters.
// Each round pairs designs with similar current scores, which converges on a useful
// ranking with far fewer matchups.
struct DuelSwissOptions {
  // Duel configuration applied to each matchup.
  //
  // Notes:
  // - duel.runs is interpreted as the number of runs per matchup direction.
  // - duel.seed is used as a base seed; each matchup derives its own deterministic seed.
  DuelOptions duel;

  // Spawn count per design per run (symmetric).
  int count_per_side{1};

  // Number of Swiss rounds to run.
  int rounds{5};

  // If true (default), each matchup is executed twice (two-way) with sides swapped to
  // reduce any spawn-side bias.
  bool two_way{true};

  // Elo ratings (optional).
  bool compute_elo{true};
  double elo_initial{1000.0};
  double elo_k_factor{32.0};
};

struct DuelSwissMatchResult {
  // Indices into DuelSwissResult::design_ids.
  int a{-1};
  int b{-1};

  // Total games accumulated for this pairing (runs * directions).
  int games{0};
  int a_wins{0};
  int b_wins{0};
  int draws{0};

  // Average simulated days per game for this matchup (0 for bye).
  double avg_days{0.0};

  // True when this entry represents a bye for player a.
  bool bye{false};
};

struct DuelSwissRoundResult {
  int round_index{0};
  std::vector<DuelSwissMatchResult> matches;
};

// Aggregate Swiss tournament result.
struct DuelSwissResult {
  std::vector<std::string> design_ids;
  DuelSwissOptions options;

  std::vector<double> elo;
  std::vector<double> points;
  std::vector<int> total_wins;
  std::vector<int> total_losses;
  std::vector<int> total_draws;
  std::vector<int> byes;

  // Simple tie-breaker: sum of opponents' final points (aka Buchholz score).
  std::vector<double> buchholz;

  std::vector<DuelSwissRoundResult> rounds;
};

// Incremental Swiss tournament runner designed for UI use.
//
// The runner mutates the supplied Simulation (as a sandbox) by repeatedly loading duel
// states. Do not point this at the player's live Simulation state.
class DuelSwissRunner {
 public:
  DuelSwissRunner(Simulation& sim, std::vector<std::string> design_ids, DuelSwissOptions options);

  bool ok() const { return ok_; }
  const std::string& error() const { return error_; }

  bool done() const { return done_; }

  int total_tasks() const { return total_tasks_; }
  int completed_tasks() const { return completed_tasks_; }
  double progress() const;

  // Human-readable label for the next matchup.
  std::string current_task_label() const;

  // Executes up to max_tasks duel tasks.
  //
  // A "task" is one matchup direction (i-vs-j). When options.two_way is enabled,
  // each matchup produces two tasks.
  void step(int max_tasks = 1);

  const DuelSwissResult& result() const { return result_; }

 private:
  struct Pairing {
    int a{-1};
    int b{-1};
  };

  void start_next_round();
  std::vector<Pairing> make_pairings_for_round();
  int choose_bye_player(const std::vector<int>& order) const;
  void finalize_result();

  Simulation& sim_;
  DuelSwissOptions options_;
  DuelSwissResult result_;

  bool ok_{true};
  bool done_{false};
  std::string error_;

  int n_{0};
  int round_{0};

  std::vector<Pairing> current_pairings_;
  int match_idx_{0};
  int dir_{0};

  int total_tasks_{0};
  int completed_tasks_{0};

  // Track which unordered pairs have already played (to avoid rematches when possible).
  std::unordered_set<std::uint64_t> played_pairs_;

  // Track opponents for Buchholz.
  std::vector<std::vector<int>> opponents_;
};

// Convenience helper that runs the entire tournament to completion.
//
// On failure, error (if provided) is filled.
DuelSwissResult run_duel_swiss(Simulation& sim,
                              const std::vector<std::string>& design_ids,
                              DuelSwissOptions options,
                              std::string* error = nullptr);

// Serialize the Swiss tournament result to JSON text.
std::string duel_swiss_to_json(const DuelSwissResult& result, int indent = 2);

}  // namespace nebula4x
