#include "nebula4x/util/duel_simulator.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <random>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "nebula4x/core/game_state.h"
#include "nebula4x/core/orders.h"
#include "nebula4x/core/scenario.h"
#include "nebula4x/core/simulation.h"
#include "nebula4x/util/digest.h"
#include "nebula4x/util/json.h"

namespace nebula4x {
namespace {

std::string hex_u64(std::uint64_t x) {
  std::ostringstream ss;
  ss << "0x" << std::hex << std::setw(16) << std::setfill('0') << x;
  return ss.str();
}

// Returns the maximum engagement range of a design, considering both beam and missile ranges.
double max_engagement_range_mkm(const ShipDesign* d) {
  if (!d) return 0.0;
  const double w = std::max(0.0, d->weapon_range_mkm);
  const double m = std::max(0.0, d->missile_range_mkm);
  return std::max(w, m);
}

// Pick a reasonable default starting separation if the caller doesn't provide one.
// We prefer to spawn within the shorter side's engagement range so both sides can act.
double choose_default_separation_mkm(const ShipDesign* da, const ShipDesign* db) {
  const double ra = max_engagement_range_mkm(da);
  const double rb = max_engagement_range_mkm(db);

  auto min_nonzero = [](double x, double y) {
    x = std::max(0.0, x);
    y = std::max(0.0, y);
    if (x <= 1e-12) return y;
    if (y <= 1e-12) return x;
    return std::min(x, y);
  };

  double r = min_nonzero(ra, rb);
  if (r <= 1e-6) r = std::max(ra, rb);

  // If neither side has a meaningful weapon range, just pick something small but non-zero.
  if (r <= 1e-6) r = 1.0;

  // Spawn slightly inside range to avoid edge flapping due to epsilons and formation offsets.
  return std::max(0.01, r * 0.8);
}

struct DuelSpawnInfo {
  Id system_id{kInvalidId};
  Id faction_a{kInvalidId};
  Id faction_b{kInvalidId};
  std::vector<Id> ships_a;
  std::vector<Id> ships_b;
};

double rand_uniform(std::mt19937& rng, double a, double b) {
  std::uniform_real_distribution<double> dist(a, b);
  return dist(rng);
}

GameState make_duel_state(Simulation& sim,
                          const DuelSideSpec& a,
                          const DuelSideSpec& b,
                          const DuelOptions& opt,
                          std::mt19937& rng,
                          DuelSpawnInfo* out_spawn) {
  GameState st;
  st.save_version = GameState{}.save_version;

  // --- system ---
  const Id sys_id = allocate_id(st);
  StarSystem sys;
  sys.id = sys_id;
  sys.name = "Duel System";
  sys.galaxy_pos = {0.0, 0.0};
  st.systems[sys_id] = sys;

  // --- factions ---
  const Id fac_a = allocate_id(st);
  const Id fac_b = allocate_id(st);

  Faction fa;
  fa.id = fac_a;
  fa.name = a.label.empty() ? "A" : a.label;
  fa.control = FactionControl::Player;

  Faction fb;
  fb.id = fac_b;
  fb.name = b.label.empty() ? "B" : b.label;
  fb.control = FactionControl::Player;

  // Explicit hostilities (avoid any ambiguity if default stance changes later).
  fa.relations[fac_b] = DiplomacyStatus::Hostile;
  fb.relations[fac_a] = DiplomacyStatus::Hostile;

  st.factions[fac_a] = fa;
  st.factions[fac_b] = fb;

  // --- ships ---
  const ShipDesign* da = sim.find_design(a.design_id);
  const ShipDesign* db = sim.find_design(b.design_id);

  double sep = opt.initial_separation_mkm;
  if (!(sep > 0.0)) sep = choose_default_separation_mkm(da, db);
  sep = std::max(0.0, sep);

  double jitter = std::max(0.0, opt.position_jitter_mkm);

  // Arrange each side in a small line, to reduce immediate pile-ups.
  // The underlying combat model doesn't simulate collisions, but spacing helps
  // keep formation offsets from causing weird initial overlaps.
  const double spacing = 0.05;

  auto add_ship = [&](Id faction_id, const std::string& prefix, int idx, double base_x, double base_y,
                      const std::string& design_id, std::vector<Id>* out_ids) {
    const Id sid = allocate_id(st);

    Ship sh;
    sh.id = sid;
    sh.name = prefix + " " + std::to_string(idx);
    sh.faction_id = faction_id;
    sh.system_id = sys_id;
    sh.design_id = design_id;
    sh.hp = 0.0;          // will be initialized to design max in load_game()
    sh.fuel_tons = -1.0;  // initialize to full if the design has fuel
    sh.shields = -1.0;    // initialize to full if the design has shields

    double x = base_x;
    double y = base_y;

    if (jitter > 1e-12) {
      x += rand_uniform(rng, -jitter, jitter);
      y += rand_uniform(rng, -jitter, jitter);
    }

    sh.position_mkm = {x, y};

    st.ships[sid] = sh;
    st.ship_orders[sid] = ShipOrders{};
    st.systems[sys_id].ships.push_back(sid);

    if (out_ids) out_ids->push_back(sid);
    return sid;
  };

  const int a_count = std::max(0, a.count);
  const int b_count = std::max(0, b.count);

  const double ax = -sep * 0.5;
  const double bx = +sep * 0.5;

  for (int i = 0; i < a_count; ++i) {
    add_ship(fac_a, (a.label.empty() ? "A" : a.label), i + 1, ax, (i - a_count * 0.5) * spacing, a.design_id,
             out_spawn ? &out_spawn->ships_a : nullptr);
  }
  for (int i = 0; i < b_count; ++i) {
    add_ship(fac_b, (b.label.empty() ? "B" : b.label), i + 1, bx, (i - b_count * 0.5) * spacing, b.design_id,
             out_spawn ? &out_spawn->ships_b : nullptr);
  }

  if (out_spawn) {
    out_spawn->system_id = sys_id;
    out_spawn->faction_a = fac_a;
    out_spawn->faction_b = fac_b;
  }

  // Issue basic AttackShip orders so ships close into range.
  if (opt.issue_attack_orders && out_spawn) {
    const Id target_b = (!out_spawn->ships_b.empty()) ? out_spawn->ships_b.front() : kInvalidId;
    const Id target_a = (!out_spawn->ships_a.empty()) ? out_spawn->ships_a.front() : kInvalidId;

    if (target_b != kInvalidId) {
      for (Id sid : out_spawn->ships_a) {
        st.ship_orders[sid].queue.push_back(AttackShip{target_b});
      }
    }
    if (target_a != kInvalidId) {
      for (Id sid : out_spawn->ships_b) {
        st.ship_orders[sid].queue.push_back(AttackShip{target_a});
      }
    }
  }

  return st;
}

struct SideTotals {
  int ships{0};
  double hp{0.0};
};

SideTotals compute_side_totals(const GameState& st, Id faction_id) {
  SideTotals t;
  for (const auto& [sid, sh] : st.ships) {
    if (sh.faction_id != faction_id) continue;
    // Only count alive ships (combat removes destroyed ships, but be defensive).
    if (sh.hp <= 0.0) continue;
    t.ships += 1;
    t.hp += sh.hp;
  }
  return t;
}

} // namespace

DuelAggregateResult run_design_duel(Simulation& sim,
                                   DuelSideSpec a,
                                   DuelSideSpec b,
                                   DuelOptions options,
                                   std::string* error) {
  DuelAggregateResult out;
  out.a = std::move(a);
  out.b = std::move(b);
  out.options = options;

  auto fail = [&](const std::string& msg) {
    if (error) *error = msg;
    // Keep the partially-filled out object, but clear runs to signal failure.
    out.runs.clear();
    out.a_wins = out.b_wins = out.draws = 0;
    out.a_win_rate = out.b_win_rate = out.draw_rate = 0.0;
    out.avg_days = out.avg_a_survivors = out.avg_b_survivors = 0.0;
    return out;
  };

  if (out.a.design_id.empty() || out.b.design_id.empty()) {
    return fail("Duel requires non-empty design ids for both sides.");
  }

  const ShipDesign* da = sim.find_design(out.a.design_id);
  const ShipDesign* db = sim.find_design(out.b.design_id);
  if (!da) return fail("Design not found for side A: '" + out.a.design_id + "'");
  if (!db) return fail("Design not found for side B: '" + out.b.design_id + "'");

  options.max_days = std::max(0, options.max_days);
  options.runs = std::max(1, options.runs);
  options.position_jitter_mkm = std::max(0.0, options.position_jitter_mkm);

  std::mt19937 rng(options.seed);

  double sum_days = 0.0;
  double sum_a_surv = 0.0;
  double sum_b_surv = 0.0;

  out.runs.reserve(options.runs);

  for (int run = 0; run < options.runs; ++run) {
    // Per-run seed is derived deterministically from the base seed.
    const std::uint32_t run_seed = static_cast<std::uint32_t>(rng());

    DuelSpawnInfo spawn;
    std::mt19937 run_rng(run_seed);

    GameState st = make_duel_state(sim, out.a, out.b, options, run_rng, &spawn);
    sim.load_game(std::move(st));

    int days_simulated = 0;

    // Early termination if one side starts empty.
    while (days_simulated < options.max_days) {
      const SideTotals ta = compute_side_totals(sim.state(), spawn.faction_a);
      const SideTotals tb = compute_side_totals(sim.state(), spawn.faction_b);

      if (ta.ships == 0 || tb.ships == 0) break;

      sim.advance_days(1);
      days_simulated += 1;
    }

    const SideTotals ta = compute_side_totals(sim.state(), spawn.faction_a);
    const SideTotals tb = compute_side_totals(sim.state(), spawn.faction_b);

    DuelRunResult rr;
    rr.run_index = run;
    rr.seed = run_seed;
    rr.days_simulated = days_simulated;
    rr.a_survivors = ta.ships;
    rr.b_survivors = tb.ships;
    rr.a_total_hp = ta.hp;
    rr.b_total_hp = tb.hp;

    if (ta.ships > 0 && tb.ships == 0) {
      rr.winner = "A";
      out.a_wins += 1;
    } else if (tb.ships > 0 && ta.ships == 0) {
      rr.winner = "B";
      out.b_wins += 1;
    } else {
      rr.winner = "Draw";
      out.draws += 1;
    }

    if (options.include_final_state_digest) {
      DigestOptions dig_opt;
      dig_opt.include_events = true;
      dig_opt.include_ui_state = false; // duel sandbox doesn't care about UI fields.
      const std::uint64_t d = digest_game_state64(sim.state(), dig_opt);
      rr.final_state_digest_hex = hex_u64(d);
    }

    sum_days += static_cast<double>(days_simulated);
    sum_a_surv += static_cast<double>(ta.ships);
    sum_b_surv += static_cast<double>(tb.ships);

    out.runs.push_back(std::move(rr));
  }

  const double denom = static_cast<double>(std::max(1, options.runs));
  out.a_win_rate = static_cast<double>(out.a_wins) / denom;
  out.b_win_rate = static_cast<double>(out.b_wins) / denom;
  out.draw_rate = static_cast<double>(out.draws) / denom;

  out.avg_days = sum_days / denom;
  out.avg_a_survivors = sum_a_surv / denom;
  out.avg_b_survivors = sum_b_surv / denom;

  return out;
}

std::string duel_result_to_json(const DuelAggregateResult& result, int indent) {
  using nebula4x::json::Value;
  using nebula4x::json::Object;
  using nebula4x::json::Array;

  auto side_to_obj = [](const DuelSideSpec& s) -> Object {
    Object o;
    o["design_id"] = Value(s.design_id);
    o["count"] = Value(static_cast<double>(s.count));
    o["label"] = Value(s.label);
    return o;
  };

  Object opt;
  opt["max_days"] = Value(static_cast<double>(result.options.max_days));
  opt["initial_separation_mkm"] = Value(result.options.initial_separation_mkm);
  opt["position_jitter_mkm"] = Value(result.options.position_jitter_mkm);
  opt["runs"] = Value(static_cast<double>(result.options.runs));
  opt["seed"] = Value(static_cast<double>(result.options.seed));
  opt["issue_attack_orders"] = Value(result.options.issue_attack_orders);
  opt["include_final_state_digest"] = Value(result.options.include_final_state_digest);

  Array runs;
  runs.reserve(result.runs.size());
  for (const auto& r : result.runs) {
    Object ro;
    ro["run_index"] = Value(static_cast<double>(r.run_index));
    ro["seed"] = Value(static_cast<double>(r.seed));
    ro["days_simulated"] = Value(static_cast<double>(r.days_simulated));
    ro["winner"] = Value(r.winner);
    ro["a_survivors"] = Value(static_cast<double>(r.a_survivors));
    ro["b_survivors"] = Value(static_cast<double>(r.b_survivors));
    ro["a_total_hp"] = Value(r.a_total_hp);
    ro["b_total_hp"] = Value(r.b_total_hp);
    ro["final_state_digest_hex"] = Value(r.final_state_digest_hex);
    runs.push_back(Value(std::move(ro)));
  }

  Object agg;
  agg["a_wins"] = Value(static_cast<double>(result.a_wins));
  agg["b_wins"] = Value(static_cast<double>(result.b_wins));
  agg["draws"] = Value(static_cast<double>(result.draws));
  agg["a_win_rate"] = Value(result.a_win_rate);
  agg["b_win_rate"] = Value(result.b_win_rate);
  agg["draw_rate"] = Value(result.draw_rate);
  agg["avg_days"] = Value(result.avg_days);
  agg["avg_a_survivors"] = Value(result.avg_a_survivors);
  agg["avg_b_survivors"] = Value(result.avg_b_survivors);

  Object root;
  root["type"] = Value("nebula4x_duel_result_v1");
  root["a"] = Value(side_to_obj(result.a));
  root["b"] = Value(side_to_obj(result.b));
  root["options"] = Value(std::move(opt));
  root["aggregate"] = Value(std::move(agg));
  root["runs"] = Value(std::move(runs));

  return nebula4x::json::stringify(Value(std::move(root)), indent);
}

} // namespace nebula4x
