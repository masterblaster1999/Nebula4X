#include "nebula4x/util/duel_swiss_tournament.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
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
  // Murmur-inspired mix (same as in duel_tournament.cpp).
  v *= 0xcc9e2d51u;
  v = (v << 15) | (v >> 17);
  v *= 0x1b873593u;
  h ^= v;
  h = (h << 13) | (h >> 19);
  h = h * 5u + 0xe6546b64u;
  return h;
}

std::uint32_t derive_task_seed(const DuelSwissOptions& opt,
                               const std::string& a,
                               const std::string& b,
                               int round,
                               int dir) {
  std::uint32_t h = static_cast<std::uint32_t>(opt.duel.seed);
  h = mix_u32(h, fnv1a32(a));
  h = mix_u32(h, fnv1a32(b));
  h = mix_u32(h, static_cast<std::uint32_t>(round));
  h = mix_u32(h, static_cast<std::uint32_t>(dir));

  // Final avalanche.
  h ^= (h >> 16);
  h *= 0x85ebca6bu;
  h ^= (h >> 13);
  h *= 0xc2b2ae35u;
  h ^= (h >> 16);
  return h;
}

std::uint64_t pair_key(int a, int b) {
  const std::uint32_t lo = static_cast<std::uint32_t>(std::min(a, b));
  const std::uint32_t hi = static_cast<std::uint32_t>(std::max(a, b));
  return (static_cast<std::uint64_t>(hi) << 32) | static_cast<std::uint64_t>(lo);
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

// Sort key used for pairing and bye selection.
struct RankKey {
  double points{0.0};
  double elo{0.0};
  int wins{0};
  int losses{0};
  int idx{0};
};

RankKey rank_key(const DuelSwissResult& r, int idx) {
  RankKey k;
  k.points = (idx >= 0 && idx < static_cast<int>(r.points.size())) ? r.points[idx] : 0.0;
  k.elo = (idx >= 0 && idx < static_cast<int>(r.elo.size())) ? r.elo[idx] : 0.0;
  k.wins = (idx >= 0 && idx < static_cast<int>(r.total_wins.size())) ? r.total_wins[idx] : 0;
  k.losses = (idx >= 0 && idx < static_cast<int>(r.total_losses.size())) ? r.total_losses[idx] : 0;
  k.idx = idx;
  return k;
}

} // namespace

DuelSwissRunner::DuelSwissRunner(Simulation& sim, std::vector<std::string> design_ids, DuelSwissOptions options)
    : sim_(sim), options_(std::move(options)) {
  // Sanitize options.
  options_.count_per_side = std::max(0, options_.count_per_side);
  options_.rounds = std::max(0, options_.rounds);
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
    error_ = "Swiss duel tournament requires at least two unique design ids.";
    done_ = true;
    return;
  }

  if (options_.rounds <= 0) {
    ok_ = false;
    error_ = "Swiss duel tournament requires rounds > 0.";
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

  // Allocate vectors.
  result_.elo.assign(n_, options_.elo_initial);
  result_.points.assign(n_, 0.0);
  result_.total_wins.assign(n_, 0);
  result_.total_losses.assign(n_, 0);
  result_.total_draws.assign(n_, 0);
  result_.byes.assign(n_, 0);
  result_.buchholz.assign(n_, 0.0);
  opponents_.assign(n_, {});

  played_pairs_.clear();
  played_pairs_.reserve(static_cast<std::size_t>(options_.rounds) * static_cast<std::size_t>(n_));

  const int matches_per_round = n_ / 2;  // floor
  total_tasks_ = options_.rounds * matches_per_round * (options_.two_way ? 2 : 1);
  completed_tasks_ = 0;

  round_ = 0;
  match_idx_ = 0;
  dir_ = 0;

  start_next_round();
}

double DuelSwissRunner::progress() const {
  if (total_tasks_ <= 0) return done_ ? 1.0 : 0.0;
  return static_cast<double>(completed_tasks_) / static_cast<double>(total_tasks_);
}

std::string DuelSwissRunner::current_task_label() const {
  if (!ok_) return "(error)";
  if (done_) return "(done)";
  if (match_idx_ < 0 || match_idx_ >= static_cast<int>(current_pairings_.size())) return "";

  const auto p = current_pairings_[match_idx_];
  if (p.a < 0 || p.a >= n_ || p.b < 0 || p.b >= n_) return "";

  int ia = p.a;
  int ib = p.b;
  bool swapped = false;
  if (options_.two_way && dir_ == 1) {
    std::swap(ia, ib);
    swapped = true;
  }

  std::string s;
  s.reserve(96);
  s += "R";
  s += std::to_string(round_ + 1);
  s += ": ";
  s += result_.design_ids[ia];
  s += " vs ";
  s += result_.design_ids[ib];
  if (swapped) s += " (swap)";
  return s;
}

int DuelSwissRunner::choose_bye_player(const std::vector<int>& order) const {
  // Choose the lowest-ranked player without a bye yet.
  // If everyone has already had a bye, pick the last in the order.
  for (auto it = order.rbegin(); it != order.rend(); ++it) {
    const int idx = *it;
    if (idx < 0 || idx >= n_) continue;
    if (result_.byes[idx] == 0) return idx;
  }
  if (!order.empty()) return order.back();
  return -1;
}

std::vector<DuelSwissRunner::Pairing> DuelSwissRunner::make_pairings_for_round() {
  // Ranking order: points desc, elo desc, wins desc, losses asc, roster order.
  std::vector<int> order(n_);
  for (int i = 0; i < n_; ++i) order[i] = i;
  std::sort(order.begin(), order.end(), [&](int a, int b) {
    const RankKey ka = rank_key(result_, a);
    const RankKey kb = rank_key(result_, b);
    if (ka.points != kb.points) return ka.points > kb.points;
    if (ka.elo != kb.elo) return ka.elo > kb.elo;
    if (ka.wins != kb.wins) return ka.wins > kb.wins;
    if (ka.losses != kb.losses) return ka.losses < kb.losses;
    return ka.idx < kb.idx;
  });

  std::vector<int> pool = order;
  std::vector<Pairing> pairings;
  pairings.reserve(static_cast<std::size_t>(n_ / 2));

  // Handle bye for odd N.
  int bye_player = -1;
  if ((n_ % 2) == 1) {
    bye_player = choose_bye_player(order);
    pool.erase(std::remove(pool.begin(), pool.end(), bye_player), pool.end());
  }

  // Track currently unpaired.
  std::vector<bool> used(n_, false);
  if (bye_player >= 0 && bye_player < n_) used[bye_player] = true;

  // Greedy pairing: for each player, pick the highest-ranked available opponent
  // they haven't already played if possible.
  for (std::size_t i = 0; i < pool.size(); ++i) {
    const int a = pool[i];
    if (a < 0 || a >= n_ || used[a]) continue;

    int best_b = -1;
    // First pass: avoid rematches.
    for (std::size_t j = i + 1; j < pool.size(); ++j) {
      const int b = pool[j];
      if (b < 0 || b >= n_ || used[b]) continue;
      const std::uint64_t key = pair_key(a, b);
      const bool played = (played_pairs_.find(key) != played_pairs_.end());
      if (!played) {
        best_b = b;
        break;
      }
    }
    // Second pass: allow rematch if unavoidable.
    if (best_b == -1) {
      for (std::size_t j = i + 1; j < pool.size(); ++j) {
        const int b = pool[j];
        if (b < 0 || b >= n_ || used[b]) continue;
        best_b = b;
        break;
      }
    }

    if (best_b == -1) {
      // Should not happen, but fail gracefully.
      continue;
    }

    used[a] = true;
    used[best_b] = true;
    pairings.push_back(Pairing{a, best_b});
  }

  // If there was a bye, store it as a pairing with b=-1 at the end.
  if (bye_player >= 0) {
    pairings.push_back(Pairing{bye_player, -1});
  }

  return pairings;
}

void DuelSwissRunner::start_next_round() {
  if (!ok_ || done_) return;
  if (round_ >= options_.rounds) {
    done_ = true;
    finalize_result();
    return;
  }

  current_pairings_.clear();
  current_pairings_ = make_pairings_for_round();
  match_idx_ = 0;
  dir_ = 0;

  DuelSwissRoundResult rr;
  rr.round_index = round_;
  rr.matches.clear();
  rr.matches.reserve(current_pairings_.size());
  result_.rounds.push_back(std::move(rr));

  // Apply byes immediately (pairings with b == -1).
  for (const auto& p : current_pairings_) {
    if (p.b != -1) continue;
    DuelSwissMatchResult m;
    m.a = p.a;
    m.b = -1;
    m.bye = true;
    m.games = 0;
    m.a_wins = 0;
    m.b_wins = 0;
    m.draws = 0;
    m.avg_days = 0.0;

    if (p.a >= 0 && p.a < n_) {
      result_.byes[p.a] += 1;
      // Swiss convention: bye counts as a win/point.
      result_.points[p.a] += 1.0;
      result_.total_wins[p.a] += 1;
    }

    result_.rounds.back().matches.push_back(std::move(m));
  }

  // Filter current_pairings_ to contain only real matches.
  current_pairings_.erase(
      std::remove_if(current_pairings_.begin(), current_pairings_.end(), [](const Pairing& p) { return p.b < 0; }),
      current_pairings_.end());
}

void DuelSwissRunner::finalize_result() {
  // Compute Buchholz (sum of opponents' final points).
  result_.buchholz.assign(n_, 0.0);
  for (int i = 0; i < n_; ++i) {
    double sum = 0.0;
    for (int opp : opponents_[i]) {
      if (opp < 0 || opp >= n_) continue;
      sum += result_.points[opp];
    }
    result_.buchholz[i] = sum;
  }
}

void DuelSwissRunner::step(int max_tasks) {
  if (!ok_ || done_) return;
  max_tasks = std::max(1, max_tasks);

  for (int t = 0; t < max_tasks && ok_ && !done_; ++t) {
    if (round_ >= options_.rounds) {
      done_ = true;
      finalize_result();
      break;
    }

    if (match_idx_ >= static_cast<int>(current_pairings_.size())) {
      // Round complete.
      round_ += 1;
      start_next_round();
      continue;
    }

    const Pairing p = current_pairings_[match_idx_];
    if (p.a < 0 || p.a >= n_ || p.b < 0 || p.b >= n_) {
      ok_ = false;
      error_ = "Invalid Swiss pairing indices.";
      done_ = true;
      return;
    }

    const int orig_a = p.a;
    const int orig_b = p.b;

    int side_a = orig_a;
    int side_b = orig_b;
    if (options_.two_way && dir_ == 1) {
      std::swap(side_a, side_b);
    }

    DuelSideSpec a;
    a.design_id = result_.design_ids[side_a];
    a.count = options_.count_per_side;
    a.label = "A";

    DuelSideSpec b;
    b.design_id = result_.design_ids[side_b];
    b.count = options_.count_per_side;
    b.label = "B";

    DuelOptions duel_opt = options_.duel;
    duel_opt.seed = derive_task_seed(options_, a.design_id, b.design_id, round_, dir_);

    std::string err;
    const auto duel_res = run_design_duel(sim_, a, b, duel_opt, &err);
    if (!err.empty() || duel_res.runs.empty()) {
      ok_ = false;
      error_ = err.empty() ? "Swiss duel task failed." : err;
      done_ = true;
      return;
    }

    // Find/create the match record for this pairing in the current round.
    auto& round_rec = result_.rounds.back();
    DuelSwissMatchResult* match_rec = nullptr;
    for (auto& m : round_rec.matches) {
      if (m.bye) continue;
      if ((m.a == orig_a && m.b == orig_b) || (m.a == orig_b && m.b == orig_a)) {
        match_rec = &m;
        break;
      }
    }
    if (!match_rec) {
      DuelSwissMatchResult m;
      m.a = orig_a;
      m.b = orig_b;
      m.bye = false;
      round_rec.matches.push_back(std::move(m));
      match_rec = &round_rec.matches.back();
    }

    int games_in_task = 0;
    int a_wins = 0;
    int b_wins = 0;
    int draws = 0;
    double sum_days = 0.0;

    for (const auto& r : duel_res.runs) {
      games_in_task += 1;
      sum_days += static_cast<double>(r.days_simulated);

      auto apply_win = [&](int winner_idx, int loser_idx) {
        result_.points[winner_idx] += 1.0;
        result_.total_wins[winner_idx] += 1;
        result_.total_losses[loser_idx] += 1;

        if (options_.compute_elo) {
          // score_a is relative to winner_idx being "A" in this call; adapt below.
        }
      };

      if (r.winner == "A") {
        const int winner = side_a;
        const int loser = side_b;
        apply_win(winner, loser);

        if (winner == orig_a) {
          a_wins += 1;
        } else {
          b_wins += 1;
        }

        if (options_.compute_elo) {
          // Elo update expects score for side_a.
          double& ra = result_.elo[side_a];
          double& rb = result_.elo[side_b];
          elo_update(ra, rb, 1.0, options_.elo_k_factor);
        }
      } else if (r.winner == "B") {
        const int winner = side_b;
        const int loser = side_a;
        apply_win(winner, loser);

        if (winner == orig_a) {
          a_wins += 1;
        } else {
          b_wins += 1;
        }

        if (options_.compute_elo) {
          double& ra = result_.elo[side_a];
          double& rb = result_.elo[side_b];
          elo_update(ra, rb, 0.0, options_.elo_k_factor);
        }
      } else {
        // Draw.
        result_.points[side_a] += 0.5;
        result_.points[side_b] += 0.5;
        result_.total_draws[side_a] += 1;
        result_.total_draws[side_b] += 1;
        draws += 1;

        if (options_.compute_elo) {
          double& ra = result_.elo[side_a];
          double& rb = result_.elo[side_b];
          elo_update(ra, rb, 0.5, options_.elo_k_factor);
        }
      }
    }

    // Update match record.
    match_rec->games += games_in_task;
    match_rec->a_wins += a_wins;
    match_rec->b_wins += b_wins;
    match_rec->draws += draws;
    // Maintain a running average days per game.
    const double prev_games = static_cast<double>(match_rec->games - games_in_task);
    const double prev_sum = match_rec->avg_days * prev_games;
    match_rec->avg_days = (prev_sum + sum_days) / static_cast<double>(match_rec->games);

    // Track opponents and played pairs once per matchup (not per direction/run).
    if (dir_ == 0) {
      opponents_[orig_a].push_back(orig_b);
      opponents_[orig_b].push_back(orig_a);
      played_pairs_.insert(pair_key(orig_a, orig_b));
    }

    completed_tasks_ += 1;

    // Advance direction/match.
    if (options_.two_way && dir_ == 0) {
      dir_ = 1;
    } else {
      dir_ = 0;
      match_idx_ += 1;
    }
  }
}

DuelSwissResult run_duel_swiss(Simulation& sim,
                              const std::vector<std::string>& design_ids,
                              DuelSwissOptions options,
                              std::string* error) {
  DuelSwissRunner runner(sim, design_ids, std::move(options));
  if (!runner.ok()) {
    if (error) *error = runner.error();
    return runner.result();
  }
  while (runner.ok() && !runner.done()) {
    runner.step(1);
  }
  if (!runner.ok()) {
    if (error) *error = runner.error();
  }
  return runner.result();
}

std::string duel_swiss_to_json(const DuelSwissResult& result, int indent) {
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
  opt["rounds"] = Value(static_cast<double>(result.options.rounds));
  opt["two_way"] = Value(result.options.two_way);
  opt["compute_elo"] = Value(result.options.compute_elo);
  opt["elo_initial"] = Value(result.options.elo_initial);
  opt["elo_k_factor"] = Value(result.options.elo_k_factor);

  Array ids;
  ids.reserve(result.design_ids.size());
  for (const auto& id : result.design_ids) ids.push_back(Value(id));

  auto vec_d = [](const std::vector<double>& v) {
    Array a;
    a.reserve(v.size());
    for (double x : v) a.push_back(Value(x));
    return a;
  };
  auto vec_i = [](const std::vector<int>& v) {
    Array a;
    a.reserve(v.size());
    for (int x : v) a.push_back(Value(static_cast<double>(x)));
    return a;
  };

  Array rounds;
  rounds.reserve(result.rounds.size());
  for (const auto& rr : result.rounds) {
    Object r;
    r["round"] = Value(static_cast<double>(rr.round_index + 1));
    Array matches;
    matches.reserve(rr.matches.size());
    for (const auto& m : rr.matches) {
      Object mm;
      if (m.a >= 0 && m.a < static_cast<int>(result.design_ids.size())) {
        mm["a"] = Value(result.design_ids[m.a]);
      } else {
        mm["a"] = Value("?");
      }
      if (m.bye || m.b < 0) {
        mm["b"] = Value(nullptr);
        mm["bye"] = Value(true);
      } else {
        mm["b"] = Value(result.design_ids[m.b]);
        mm["bye"] = Value(false);
      }
      mm["games"] = Value(static_cast<double>(m.games));
      mm["a_wins"] = Value(static_cast<double>(m.a_wins));
      mm["b_wins"] = Value(static_cast<double>(m.b_wins));
      mm["draws"] = Value(static_cast<double>(m.draws));
      mm["avg_days"] = Value(m.avg_days);
      matches.push_back(Value(std::move(mm)));
    }
    r["matches"] = Value(std::move(matches));
    rounds.push_back(Value(std::move(r)));
  }

  Object out;
  out["options"] = Value(std::move(opt));
  out["design_ids"] = Value(std::move(ids));
  out["elo"] = Value(vec_d(result.elo));
  out["points"] = Value(vec_d(result.points));
  out["buchholz"] = Value(vec_d(result.buchholz));
  out["wins"] = Value(vec_i(result.total_wins));
  out["losses"] = Value(vec_i(result.total_losses));
  out["draws"] = Value(vec_i(result.total_draws));
  out["byes"] = Value(vec_i(result.byes));
  out["rounds"] = Value(std::move(rounds));

  return nebula4x::json::stringify(Value(std::move(out)), indent) + "\n";
}

}  // namespace nebula4x
