#include <cmath>
#include <iostream>
#include <string>

#include "nebula4x/core/simulation.h"

#define N4X_ASSERT(expr)                                                                            \
  do {                                                                                              \
    if (!(expr)) {                                                                                  \
      std::cerr << "ASSERT failed: " #expr " (" << __FILE__ << ":" << __LINE__ << ")\n";          \
      return 1;                                                                                     \
    }                                                                                               \
  } while (0)

namespace {

nebula4x::ContentDB make_content() {
  nebula4x::ContentDB content;
  nebula4x::InstallationDef mine;
  mine.id = "automated_mine";
  mine.name = "Automated Mine";
  mine.produces_per_day = {{"Duranium", 10.0}};
  mine.mining = true;
  content.installations[mine.id] = mine;
  return content;
}

nebula4x::GameState make_state(double stock_a, double stock_b) {
  nebula4x::GameState s;
  s.save_version = nebula4x::GameState{}.save_version;
  s.date = nebula4x::Date::from_ymd(2200, 1, 1);

  const nebula4x::Id sys_id = 1;
  const nebula4x::Id body_id = 2;
  const nebula4x::Id faction_id = 3;
  const nebula4x::Id colony_a_id = 10;
  const nebula4x::Id colony_b_id = 11;

  nebula4x::StarSystem sys;
  sys.id = sys_id;
  sys.name = "Scarcity System";
  s.systems[sys.id] = sys;

  nebula4x::Body body;
  body.id = body_id;
  body.name = "Shared Rock";
  body.type = nebula4x::BodyType::Planet;
  body.system_id = sys_id;
  body.mineral_deposits["Duranium"] = 10.0;
  s.bodies[body.id] = body;

  nebula4x::Faction fac;
  fac.id = faction_id;
  fac.name = "Player";
  fac.control = nebula4x::FactionControl::Player;
  s.factions[fac.id] = fac;

  nebula4x::Colony a;
  a.id = colony_a_id;
  a.name = "Alpha";
  a.faction_id = faction_id;
  a.body_id = body_id;
  a.population_millions = 0.0;
  a.installations["automated_mine"] = 1;
  a.minerals["Duranium"] = stock_a;
  s.colonies[a.id] = a;

  nebula4x::Colony b;
  b.id = colony_b_id;
  b.name = "Beta";
  b.faction_id = faction_id;
  b.body_id = body_id;
  b.population_millions = 0.0;
  b.installations["automated_mine"] = 1;
  b.minerals["Duranium"] = stock_b;
  s.colonies[b.id] = b;

  s.next_id = 100;
  return s;
}

double colony_mineral(const nebula4x::Simulation& sim, nebula4x::Id colony_id, const std::string& mineral) {
  const auto* c = nebula4x::find_ptr(sim.state().colonies, colony_id);
  if (!c) return 0.0;
  const auto it = c->minerals.find(mineral);
  return (it == c->minerals.end()) ? 0.0 : it->second;
}

} // namespace

int test_mining_scarcity_allocation() {
  const nebula4x::Id colony_a_id = 10;
  const nebula4x::Id colony_b_id = 11;
  const nebula4x::Id body_id = 2;

  // Scarcity-priority mode should bias finite extraction toward the colony with
  // the lower local stockpile.
  {
    nebula4x::SimConfig cfg;
    cfg.enable_colony_stability_output_scaling = false;
    cfg.enable_colony_conditions = false;
    cfg.enable_trade_prosperity = false;
    cfg.enable_blockades = false;
    cfg.enable_mining_scarcity_priority = true;
    cfg.mining_scarcity_buffer_days = 20.0;
    cfg.mining_scarcity_need_boost = 2.0;

    nebula4x::Simulation sim(make_content(), cfg);
    sim.load_game(make_state(/*stock_a=*/0.0, /*stock_b=*/200.0));

    const double a0 = colony_mineral(sim, colony_a_id, "Duranium");
    const double b0 = colony_mineral(sim, colony_b_id, "Duranium");
    sim.advance_days(1);
    const double a1 = colony_mineral(sim, colony_a_id, "Duranium");
    const double b1 = colony_mineral(sim, colony_b_id, "Duranium");

    const double a_gain = a1 - a0;
    const double b_gain = b1 - b0;

    N4X_ASSERT(a_gain > b_gain);
    N4X_ASSERT(a_gain > 5.0);
    N4X_ASSERT(b_gain < 5.0);
    N4X_ASSERT(std::fabs((a_gain + b_gain) - 10.0) < 1e-6);

    const auto* body = nebula4x::find_ptr(sim.state().bodies, body_id);
    N4X_ASSERT(body);
    N4X_ASSERT(std::fabs(body->mineral_deposits.at("Duranium")) < 1e-9);
  }

  // Legacy mode remains proportional-by-request.
  {
    nebula4x::SimConfig cfg;
    cfg.enable_colony_stability_output_scaling = false;
    cfg.enable_colony_conditions = false;
    cfg.enable_trade_prosperity = false;
    cfg.enable_blockades = false;
    cfg.enable_mining_scarcity_priority = false;
    cfg.mining_scarcity_buffer_days = 20.0;
    cfg.mining_scarcity_need_boost = 2.0;

    nebula4x::Simulation sim(make_content(), cfg);
    sim.load_game(make_state(/*stock_a=*/0.0, /*stock_b=*/200.0));

    const double a0 = colony_mineral(sim, colony_a_id, "Duranium");
    const double b0 = colony_mineral(sim, colony_b_id, "Duranium");
    sim.advance_days(1);
    const double a1 = colony_mineral(sim, colony_a_id, "Duranium");
    const double b1 = colony_mineral(sim, colony_b_id, "Duranium");

    const double a_gain = a1 - a0;
    const double b_gain = b1 - b0;
    N4X_ASSERT(std::fabs(a_gain - 5.0) < 1e-6);
    N4X_ASSERT(std::fabs(b_gain - 5.0) < 1e-6);
  }

  return 0;
}
