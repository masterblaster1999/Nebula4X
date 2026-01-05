#include "nebula4x/util/duel_tournament.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "nebula4x/core/simulation.h"
#include "nebula4x/util/json.h"

namespace nebula4x {
namespace {

std::uint32_t fnv1a32(const std::string& s) {
  std::uint32_t h = 2166136261u;
  for (unsigned char c : s) {
    h ^= static_cast<std::uint32_t>(c);
    h *= 16777619u;
  }
  return h;
}

std::uint32_t mix_u32(std::uint32_t h, std::uint32_t v) {
  // Murmur-inspired mix.
  v *= 0xcc9e2d51u;
  v = (v << 15) | (v >> 17);
  v *= 0x1b873593u;
  h ^= v;
  h = (h << 13) | (h >> 19);
  h = h * 5u + 0xe6546b64u;
  return h;
}

std::uint32_t derive_task_seed(const DuelRoundRobinOptions& opt,
                               const std::string& a,
                               const std::string& b,
                               int dir) {
  std::uint32_t h = static_cast<std::uint32_t>(opt.duel.seed);
  h = mix_u32(h, fnv1a32(a));
  h = mix_u32(h, fnv1a32(b));
  h = mix_u32(h, static_cast<std::uint32_t>(dir));
  // Final avalanche.
  h ^= (h >> 16);
  h *= 0x85ebca6bu;
  h ^= (h >> 13);
  h *= 0xc2b2ae35u;
  h ^= (h >> 16);
  return h;
}

double elo_expected(double ra, double rb) {
  const double diff = (rb - ra) / 400.0;
  return 1.0 / (1.0 + std::pow(10.0, diff));
}

void elo_update(double& ra, double& rb, double score_a, double k) {
  const double ea = elo_expected(ra, rb);
  const double eb = 1.0 - ea;
  ra = ra + k * (score_a - ea);
  rb = rb + k * ((1.0 - score_a) - eb);
}

} // namespace

DuelRoundRobinRunner::DuelRoundRobinRunner(Simulation& sim,
                                           std::vector<std::string> design_ids,
                                           DuelRoundRobinOptions options)
    : sim_(sim), options_(std::move(options)) {
  // Sanitize options.
  options_.count_per_side = std::max(0, options_.count_per_side);
  options_.duel.max_days = std::max(0, options_.duel.max_days);
  options_.duel.runs = std::max(1, options_.duel.runs);
  options_.duel.position_jitter_mkm = std::max(0.0, options_.duel.position_jitter_mkm);
  options_.elo_k_factor = std::clamp(options_.elo_k_factor, 0.0, 400.0);

  // De-duplicate while preserving the caller's order.
  {
    std::vector<std::string> uniq;
    uniq.reserve(design_ids.size());
    std::unordered_set<std::string> seen;
    seen.reserve(design_ids.size() * 2);
    for (auto& id : design_ids) {
      if (id.empty()) continue;
      if (seen.insert(id).second) uniq.push_back(std::move(id));
    }
    design_ids = std::move(uniq);
  }

  result_.design_ids = std::move(design_ids);
  result_.options = options_;
  n_ = static_cast<int>(result_.design_ids.size());

  if (n_ < 2) {
    ok_ = false;
    error_ = "Round-robin duel requires at least two unique design ids.";
    done_ = true;
    return;
  }

  // Validate designs exist.
  for (const auto& id : result_.design_ids) {
    if (!sim_.find_design(id)) {
      ok_ = false;
      error_ = "Design not found: '" + id + "'";
      done_ = true;
      return;
    }
  }

  // Allocate matrices.
  result_.elo.assign(n_, options_.elo_initial);
  result_.total_wins.assign(n_, 0);
  result_.total_losses.assign(n_, 0);
  result_.total_draws.assign(n_, 0);

  result_.games.assign(n_, std::vector<int>(n_, 0));
  result_.wins.assign(n_, std::vector<int>(n_, 0));
  result_.draws.assign(n_, std::vector<int>(n_, 0));
  result_.sum_days.assign(n_, std::vector<double>(n_, 0.0));

  i_ = 0;
  j_ = 1;
  dir_ = 0;

  const int pairs = (n_ * (n_ - 1)) / 2;
  total_tasks_ = pairs * (options_.two_way ? 2 : 1);
  completed_tasks_ = 0;
  done_ = (total_tasks_ == 0);
}

double DuelRoundRobinRunner::progress() const {
  if (total_tasks_ <= 0) return done_ ? 1.0 : 0.0;
  return static_cast<double>(completed_tasks_) / static_cast<double>(total_tasks_);
}

std::string DuelRoundRobinRunner::current_task_label() const {
  if (n_ < 2) return std::string{};
  if (done_) return "(done)";

  int ia = i_;
  int ib = j_;
  bool swapped = false;
  if (options_.two_way && dir_ == 1) {
    std::swap(ia, ib);
    swapped = true;
  }

  std::string s;
  s.reserve(64);
  s += result_.design_ids[ia];
  s += " vs ";
  s += result_.design_ids[ib];
  if (swapped) s += " (swap)";
  return s;
}

void DuelRoundRobinRunner::step(int max_tasks) {
  if (!ok_ || done_) return;
  max_tasks = std::max(1, max_tasks);

  for (int t = 0; t < max_tasks && ok_ && !done_; ++t) {
    int ia = i_;
    int ib = j_;
    if (options_.two_way && dir_ == 1) {
      std::swap(ia, ib);
    }

    // Configure and run one duel task.
    DuelSideSpec a;
    a.design_id = result_.design_ids[ia];
    a.count = options_.count_per_side;
    a.label = "A";

    DuelSideSpec b;
    b.design_id = result_.design_ids[ib];
    b.count = options_.count_per_side;
    b.label = "B";

    DuelOptions duel_opt = options_.duel;
    duel_opt.seed = derive_task_seed(options_, a.design_id, b.design_id, dir_);

    std::string err;
    const auto duel_res = run_design_duel(sim_, a, b, duel_opt, &err);
    if (!err.empty() || duel_res.runs.empty()) {
      ok_ = false;
      error_ = err.empty() ? "Duel task failed." : err;
      done_ = true;
      return;
    }

    // Consume per-run outcomes.
    for (const auto& r : duel_res.runs) {
      // Update game counters.
      result_.games[ia][ib] += 1;
      result_.games[ib][ia] += 1;
      result_.sum_days[ia][ib] += static_cast<double>(r.days_simulated);
      result_.sum_days[ib][ia] += static_cast<double>(r.days_simulated);

      if (r.winner == "A") {
        result_.wins[ia][ib] += 1;
        result_.total_wins[ia] += 1;
        result_.total_losses[ib] += 1;

        if (options_.compute_elo) {
          elo_update(result_.elo[ia], result_.elo[ib], 1.0, options_.elo_k_factor);
        }
      } else if (r.winner == "B") {
        result_.wins[ib][ia] += 1;
        result_.total_wins[ib] += 1;
        result_.total_losses[ia] += 1;

        if (options_.compute_elo) {
          elo_update(result_.elo[ia], result_.elo[ib], 0.0, options_.elo_k_factor);
        }
      } else {
        result_.draws[ia][ib] += 1;
        result_.draws[ib][ia] += 1;
        result_.total_draws[ia] += 1;
        result_.total_draws[ib] += 1;

        if (options_.compute_elo) {
          elo_update(result_.elo[ia], result_.elo[ib], 0.5, options_.elo_k_factor);
        }
      }
    }

    completed_tasks_ += 1;

    // Advance to next task.
    if (options_.two_way && dir_ == 0) {
      dir_ = 1;
    } else {
      dir_ = 0;
      j_ += 1;
      if (j_ >= n_) {
        i_ += 1;
        j_ = i_ + 1;
      }
      if (i_ >= n_ - 1) {
        done_ = true;
      }
    }
  }
}

DuelRoundRobinResult run_duel_round_robin(Simulation& sim,
                                         const std::vector<std::string>& design_ids,
                                         DuelRoundRobinOptions options,
                                         std::string* error) {
  DuelRoundRobinRunner runner(sim, design_ids, std::move(options));
  if (!runner.ok()) {
    if (error) *error = runner.error();
    return runner.result();
  }
  // Run in moderate chunks to avoid pathological long loops in debug builds.
  while (runner.ok() && !runner.done()) {
    runner.step(1);
  }
  if (!runner.ok()) {
    if (error) *error = runner.error();
  }
  return runner.result();
}

std::string duel_round_robin_to_json(const DuelRoundRobinResult& result, int indent) {
  using nebula4x::json::Array;
  using nebula4x::json::Object;
  using nebula4x::json::Value;

  Object opt;
  {
    Object duel;
    duel["max_days"] = Value(static_cast<double>(result.options.duel.max_days));
    duel["initial_separation_mkm"] = Value(result.options.duel.initial_separation_mkm);
    duel["position_jitter_mkm"] = Value(result.options.duel.position_jitter_mkm);
    duel["runs"] = Value(static_cast<double>(result.options.duel.runs));
    duel["seed"] = Value(static_cast<double>(result.options.duel.seed));
    duel["issue_attack_orders"] = Value(result.options.duel.issue_attack_orders);
    duel["include_final_state_digest"] = Value(result.options.duel.include_final_state_digest);
    opt["duel"] = Value(std::move(duel));
  }

  opt["count_per_side"] = Value(static_cast<double>(result.options.count_per_side));
  opt["two_way"] = Value(result.options.two_way);
  opt["compute_elo"] = Value(result.options.compute_elo);
  opt["elo_initial"] = Value(result.options.elo_initial);
  opt["elo_k_factor"] = Value(result.options.elo_k_factor);

  Array ids;
  ids.reserve(result.design_ids.size());
  for (const auto& id : result.design_ids) ids.push_back(Value(id));

  Array elo;
  elo.reserve(result.elo.size());
  for (double r : result.elo) elo.push_back(Value(r));

  auto vec_i = [](const std::vector<int>& v) {
    Array a;
    a.reserve(v.size());
    for (int x : v) a.push_back(Value(static_cast<double>(x)));
    return a;
  };

  auto mat_i = [&](const std::vector<std::vector<int>>& m) {
    Array a;
    a.reserve(m.size());
    for (const auto& row : m) a.push_back(Value(vec_i(row)));
    return a;
  };

  auto mat_d = [&](const std::vector<std::vector<double>>& m) {
    Array a;
    a.reserve(m.size());
    for (const auto& row : m) {
      Array r;
      r.reserve(row.size());
      for (double x : row) r.push_back(Value(x));
      a.push_back(Value(std::move(r)));
    }
    return a;
  };

  Object totals;
  totals["wins"] = Value(vec_i(result.total_wins));
  totals["losses"] = Value(vec_i(result.total_losses));
  totals["draws"] = Value(vec_i(result.total_draws));

  Object matrices;
  matrices["games"] = Value(mat_i(result.games));
  matrices["wins"] = Value(mat_i(result.wins));
  matrices["draws"] = Value(mat_i(result.draws));

  // Also include summed days (handy for performance comparisons).
  matrices["sum_days"] = Value(mat_d(result.sum_days));

  Object root;
  root["type"] = Value("nebula4x_duel_round_robin_v1");
  root["design_ids"] = Value(std::move(ids));
  root["options"] = Value(std::move(opt));
  root["elo"] = Value(std::move(elo));
  root["totals"] = Value(std::move(totals));
  root["matrices"] = Value(std::move(matrices));

  return nebula4x::json::stringify(Value(std::move(root)), indent);
}

}  // namespace nebula4x
