#include <cmath>
#include <iostream>

#include "nebula4x/core/simulation.h"

#define N4X_ASSERT(cond, msg)                                                       \
  do {                                                                              \
    if (!(cond)) {                                                                  \
      std::cerr << "ASSERT FAIL: " << (msg) << " (" << __FILE__ << ":" << __LINE__ \
                << ")\n";                                                        \
      return 1;                                                                     \
    }                                                                               \
  } while (0)

int test_civilian_trade_activity_prosperity() {
  using namespace nebula4x;

  // Minimal content for trade goods classification.
  ContentDB content;
  auto add_res = [&](const std::string& id, const std::string& cat, bool mineable) {
    ResourceDef r;
    r.id = id;
    r.name = id;
    r.category = cat;
    r.mineable = mineable;
    content.resources[id] = r;
  };

  add_res("Duranium", "metal", true);
  add_res("Metals", "metal", false);
  add_res("Minerals", "mineral", false);
  add_res("Fuel", "fuel", false);
  add_res("Munitions", "munitions", false);

  SimConfig cfg;
  cfg.enable_trade_prosperity = true;
  cfg.enable_civilian_trade_activity_prosperity = true;
  cfg.civilian_trade_activity_score_scale_tons = 1000.0;
  cfg.civilian_trade_activity_hub_score_bonus_cap = 0.50;
  cfg.civilian_trade_activity_market_size_bonus_cap = 0.50;

  Simulation sim(content, cfg);
  sim.new_game();

  GameState st;
  st.date = Date{0};

  // Faction (needed for treaty lookup in TradeProsperityStatus).
  Faction f;
  f.id = 1;
  f.name = "Test Faction";
  f.control = FactionControl::Player;
  st.factions[f.id] = f;

  // A small 3-system chain A<->B<->C.
  StarSystem a;
  a.id = 10;
  a.name = "A";
  a.galaxy_pos = Vec2{-10.0, 0.0};
  st.systems[a.id] = a;

  StarSystem b;
  b.id = 11;
  b.name = "B";
  b.galaxy_pos = Vec2{0.0, 0.0};
  st.systems[b.id] = b;

  StarSystem c;
  c.id = 12;
  c.name = "C";
  c.galaxy_pos = Vec2{10.0, 0.0};
  st.systems[c.id] = c;

  // Bodies.
  Body a_body;
  a_body.id = 100;
  a_body.system_id = a.id;
  a_body.type = BodyType::Planet;
  a_body.surface_temp_k = 288.0;
  a_body.atmosphere_atm = 1.0;
  a_body.mass_earths = 1.0;
  a_body.radius_km = 6371.0;
  st.bodies[a_body.id] = a_body;
  st.systems[a.id].bodies.push_back(a_body.id);

  Body b_body;
  b_body.id = 101;
  b_body.system_id = b.id;
  b_body.type = BodyType::Planet;
  b_body.surface_temp_k = 288.0;
  b_body.atmosphere_atm = 1.0;
  b_body.mass_earths = 1.0;
  b_body.radius_km = 6371.0;
  st.bodies[b_body.id] = b_body;
  st.systems[b.id].bodies.push_back(b_body.id);

  Body c_body;
  c_body.id = 102;
  c_body.system_id = c.id;
  c_body.type = BodyType::Planet;
  c_body.surface_temp_k = 288.0;
  c_body.atmosphere_atm = 1.0;
  c_body.mass_earths = 1.0;
  c_body.radius_km = 6371.0;
  st.bodies[c_body.id] = c_body;
  st.systems[c.id].bodies.push_back(c_body.id);

  // Jump points A<->B and B<->C.
  JumpPoint jp_ab;
  jp_ab.id = 200;
  jp_ab.system_id = a.id;
  jp_ab.position_mkm = Vec2{0, 0};
  jp_ab.linked_jump_id = 201;
  st.jump_points[jp_ab.id] = jp_ab;
  st.systems[a.id].jump_points.push_back(jp_ab.id);

  JumpPoint jp_ba;
  jp_ba.id = 201;
  jp_ba.system_id = b.id;
  jp_ba.position_mkm = Vec2{0, 0};
  jp_ba.linked_jump_id = 200;
  st.jump_points[jp_ba.id] = jp_ba;
  st.systems[b.id].jump_points.push_back(jp_ba.id);

  JumpPoint jp_bc;
  jp_bc.id = 202;
  jp_bc.system_id = b.id;
  jp_bc.position_mkm = Vec2{0, 0};
  jp_bc.linked_jump_id = 203;
  st.jump_points[jp_bc.id] = jp_bc;
  st.systems[b.id].jump_points.push_back(jp_bc.id);

  JumpPoint jp_cb;
  jp_cb.id = 203;
  jp_cb.system_id = c.id;
  jp_cb.position_mkm = Vec2{0, 0};
  jp_cb.linked_jump_id = 202;
  st.jump_points[jp_cb.id] = jp_cb;
  st.systems[c.id].jump_points.push_back(jp_cb.id);

  // One colony in the edge system (A) so hub score isn't already 1.
  Colony col;
  col.id = 300;
  col.name = "Edge Colony";
  col.faction_id = f.id;
  col.body_id = a_body.id;
  col.population_millions = 500.0;
  col.minerals["Duranium"] = 10000.0;
  st.colonies[col.id] = col;

  sim.load_game(std::move(st));

  const TradeProsperityStatus base = sim.trade_prosperity_status_for_colony(col.id);
  N4X_ASSERT(std::isfinite(base.market_size) && base.market_size >= 0.0, "base market_size finite");
  N4X_ASSERT(std::isfinite(base.hub_score) && base.hub_score >= 0.0, "base hub_score finite");

  // Inject a large activity score and advance day to force cache refresh.
  sim.state().systems[a.id].civilian_trade_activity_score = 5000.0;
  sim.state().date = sim.state().date.add_days(1);

  const TradeProsperityStatus boosted = sim.trade_prosperity_status_for_colony(col.id);

  N4X_ASSERT(boosted.market_size + 1e-9 >= base.market_size, "market_size should not decrease with activity");
  N4X_ASSERT(boosted.hub_score + 1e-9 >= base.hub_score, "hub_score should not decrease with activity");
  N4X_ASSERT(boosted.effective_market_size + 1e-9 >= base.effective_market_size,
             "effective_market_size should not decrease with activity");

  // The caps and injected score are large enough that we expect a visible increase.
  N4X_ASSERT(boosted.hub_score >= base.hub_score + 0.01 || base.hub_score > 0.99,
             "expected hub_score to increase (unless already saturated)");

  // Verify that the activity query reports a meaningful factor.
  const auto act = sim.civilian_trade_activity_status_for_system(a.id);
  N4X_ASSERT(act.score > 0.0, "activity score should be > 0");
  N4X_ASSERT(act.factor > 0.0 && act.factor <= 1.0 + 1e-9, "activity factor should be in (0,1]");

  return 0;
}
