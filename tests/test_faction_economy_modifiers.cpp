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

  return 0;
}
