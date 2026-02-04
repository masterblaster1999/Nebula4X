#include <cmath>
#include <iostream>
#include <string>

#include "nebula4x/core/simulation.h"

#define N4X_ASSERT(expr)                                                                            \
  do {                                                                                              \
    if (!(expr)) {                                                                                  \
      std::cerr << "ASSERT failed: " #expr " (" << __FILE__ << ":" << __LINE__ << ")\n";     \
      return 1;                                                                                     \
    }                                                                                               \
  } while (0)

int test_faction_economy_modifiers() {
  using namespace nebula4x;

  ContentDB content;

  // A simple mining installation.
  {
    InstallationDef mine;
    mine.id = "automated_mine";
    mine.name = "Automated Mine";
    mine.produces_per_day = {{"Duranium", 10.0}};
    mine.mining = true;
    content.installations[mine.id] = mine;
  }

  // A simple research lab.
  {
    InstallationDef lab;
    lab.id = "research_lab";
    lab.name = "Research Lab";
    lab.research_points_per_day = 10.0;
    content.installations[lab.id] = lab;
  }

  // A simple industry building (non-mining) that produces fuel.
  {
    InstallationDef fac;
    fac.id = "fuel_plant";
    fac.name = "Fuel Plant";
    fac.produces_per_day = {{"Fuel", 10.0}};
    fac.mining = false;
    content.installations[fac.id] = fac;
  }

  // Techs that apply faction-wide output multipliers.
  {
    TechDef t;
    t.id = "mining_bonus";
    t.name = "Mining Bonus";
    t.cost = 0.0;
    t.effects.push_back(TechEffect{"faction_output_bonus", "mining", 0.50});  // +50%
    content.techs[t.id] = t;
  }
  {
    TechDef t;
    t.id = "research_bonus";
    t.name = "Research Bonus";
    t.cost = 0.0;
    t.effects.push_back(TechEffect{"faction_output_bonus", "research", 1.00});  // +100%
    content.techs[t.id] = t;
  }
  {
    TechDef t;
    t.id = "industry_bonus";
    t.name = "Industry Bonus";
    t.cost = 0.0;
    t.effects.push_back(TechEffect{"faction_output_bonus", "industry", 1.00});  // +100%
    content.techs[t.id] = t;
  }

  Simulation sim(std::move(content), SimConfig{});

  // Minimal state.
  GameState s;
  s.save_version = GameState{}.save_version;
  s.date = Date::from_ymd(2200, 1, 1);

  const Id sys_id = 1;
  {
    StarSystem sys;
    sys.id = sys_id;
    sys.name = "Test System";
    s.systems[sys.id] = sys;
  }

  const Id body_id = 2;
  {
    Body b;
    b.id = body_id;
    b.name = "Test Planet";
    b.type = BodyType::Planet;
    b.system_id = sys_id;
    b.orbit_radius_mkm = 0.0;
    b.orbit_period_days = 1.0;
    b.orbit_phase_radians = 0.0;
    b.mineral_deposits = {{"Duranium", 30.0}};
    s.bodies[b.id] = b;
  }

  const Id faction_id = 3;
  {
    Faction f;
    f.id = faction_id;
    f.name = "Test Faction";
    f.control = FactionControl::Player;
    f.known_techs = {"mining_bonus", "research_bonus", "industry_bonus"};
    s.factions[f.id] = f;
  }

  const Id colony_id = 4;
  {
    Colony c;
    c.id = colony_id;
    c.name = "Test Colony";
    c.faction_id = faction_id;
    c.body_id = body_id;
    c.population_millions = 0.0;
    c.installations = {{"automated_mine", 1}, {"research_lab", 1}, {"fuel_plant", 1}};
    s.colonies[c.id] = c;
  }

  s.next_id = 100;

  sim.load_game(std::move(s));

  // Day 1: mining 10 * 1.5 = 15; fuel plant 10 * 2 = 20; research 10 * 2 = 20.
  sim.advance_days(1);
  {
    const auto* body = find_ptr(sim.state().bodies, body_id);
    const auto* col = find_ptr(sim.state().colonies, colony_id);
    const auto* fac = find_ptr(sim.state().factions, faction_id);
    N4X_ASSERT(body && col && fac);

    N4X_ASSERT(std::abs(col->minerals.at("Duranium") - 15.0) < 1e-9);
    N4X_ASSERT(std::abs(body->mineral_deposits.at("Duranium") - 15.0) < 1e-9);

    N4X_ASSERT(std::abs(col->minerals.at("Fuel") - 20.0) < 1e-9);

    N4X_ASSERT(std::abs(fac->research_points - 20.0) < 1e-9);
  }

  // Day 2: mine should deplete the deposit (another 15 extracted).
  sim.advance_days(1);
  {
    const auto* body = find_ptr(sim.state().bodies, body_id);
    const auto* col = find_ptr(sim.state().colonies, colony_id);
    N4X_ASSERT(body && col);

    N4X_ASSERT(std::abs(col->minerals.at("Duranium") - 30.0) < 1e-9);
    N4X_ASSERT(std::abs(body->mineral_deposits.at("Duranium")) < 1e-9);
  }

  // Depletion should generate a warning event.
  N4X_ASSERT(sim.state().events.size() == 1);
  {
    const auto& ev = sim.state().events.back();
    N4X_ASSERT(ev.level == EventLevel::Warn);
    N4X_ASSERT(ev.category == EventCategory::Construction);
    N4X_ASSERT(ev.colony_id == colony_id);
    N4X_ASSERT(ev.message.find("Mineral deposit depleted") != std::string::npos);
  }

  // --- Trade agreement economy bonus ---
  //
  // A trade agreement should provide a small, deterministic bonus to
  // economic output (research/industry/shipyard/construction). This ensures
  // diplomacy has tangible economic consequences.
  {
    ContentDB content_trade;
    InstallationDef lab;
    lab.id = "research_lab";
    lab.name = "Research Lab";
    lab.research_points_per_day = 10.0;
    content_trade.installations[lab.id] = lab;

    Simulation sim_trade(content_trade, SimConfig{});
    GameState st;
    st.save_version = GameState{}.save_version;

    Faction fa;
    fa.id = 1;
    fa.name = "A";
    fa.control = FactionControl::Player;
    st.factions[fa.id] = fa;

    Faction fb;
    fb.id = 2;
    fb.name = "B";
    fb.control = FactionControl::AI_Passive;
    st.factions[fb.id] = fb;

    StarSystem sys;
    sys.id = 1;
    sys.name = "Sys";
    st.systems[sys.id] = sys;

    Body body;
    body.id = 1;
    body.name = "Planet";
    body.type = BodyType::Planet;
    body.system_id = sys.id;
    body.orbit_radius_mkm = 0.0;
    body.orbit_period_days = 1.0;
    body.orbit_phase_radians = 0.0;
    st.bodies[body.id] = body;
    st.systems[sys.id].bodies.push_back(body.id);

    Colony col;
    col.id = 1;
    col.name = "Col";
    col.faction_id = fa.id;
    col.body_id = body.id;
    col.installations = {{"research_lab", 1}};
    st.colonies[col.id] = col;

    st.next_id = 100;

    sim_trade.load_game(std::move(st));

    std::string err;
    const Id tid = sim_trade.create_treaty(fa.id, fb.id, TreatyType::TradeAgreement, /*duration_days=*/-1,
                                           /*push_event=*/false, &err);
    N4X_ASSERT(tid != kInvalidId);

    sim_trade.advance_days(1);
    const auto& fac_out = sim_trade.state().factions.at(fa.id);
    // 10 RP/day * (1 + 0.05 per trade partner) = 10.5
    N4X_ASSERT(std::abs(fac_out.research_points - 10.5) < 1e-9);
  }

  // --- Procedural trait multipliers ---
  //
  // Faction traits should scale economic output in a simple, deterministic way.
  {
    ContentDB trait_content;
    InstallationDef mine;
    mine.id = "automated_mine";
    mine.name = "Automated Mine";
    mine.produces_per_day = {{"Duranium", 10.0}};
    trait_content.installations[mine.id] = mine;

    Simulation sim_trait(std::move(trait_content), SimConfig{});
    GameState st;
    st.save_version = GameState{}.save_version;

    StarSystem sys;
    sys.id = 1;
    sys.name = "Sys";
    st.systems[sys.id] = sys;

    Body b;
    b.id = 1;
    b.name = "Asteroid";
    b.type = BodyType::Asteroid;
    b.system_id = sys.id;
    b.orbit_radius_mkm = 0.0;
    b.orbit_period_days = 1.0;
    b.orbit_phase_radians = 0.0;
    // Empty mineral deposits => unlimited extraction in the legacy mining model.
    st.bodies[b.id] = b;
    st.systems[sys.id].bodies.push_back(b.id);

    Faction f;
    f.id = 1;
    f.name = "Trait Faction";
    f.control = FactionControl::Player;
    f.traits.mining = 1.2;
    st.factions[f.id] = f;

    Colony col;
    col.id = 1;
    col.name = "Col";
    col.faction_id = f.id;
    col.body_id = b.id;
    col.installations = {{"automated_mine", 1}};
    st.colonies[col.id] = col;

    st.next_id = 10;
    sim_trait.load_game(std::move(st));
    sim_trait.advance_days(1);

    const auto* col_out = find_ptr(sim_trait.state().colonies, col.id);
    N4X_ASSERT(col_out);
    N4X_ASSERT(std::abs(col_out->minerals.at("Duranium") - 12.0) < 1e-9);
  }

  return 0;
}
