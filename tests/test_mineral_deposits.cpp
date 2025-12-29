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

int test_mineral_deposits() {
  nebula4x::ContentDB content;

  // A simple mining installation.
  nebula4x::InstallationDef mine;
  mine.id = "automated_mine";
  mine.name = "Automated Mine";
  mine.produces_per_day = {{"Duranium", 10.0}};
  mine.mining = true;
  content.installations[mine.id] = mine;

  nebula4x::Simulation sim(std::move(content), nebula4x::SimConfig{});

  // Replace the default Sol scenario with a minimal custom state.
  nebula4x::GameState s;
  s.save_version = nebula4x::GameState{}.save_version;
  s.date = nebula4x::Date::from_ymd(2200, 1, 1);

  const nebula4x::Id sys_id = 1;
  {
    nebula4x::StarSystem sys;
    sys.id = sys_id;
    sys.name = "Test System";
    s.systems[sys.id] = sys;
  }

  const nebula4x::Id body_id = 2;
  {
    nebula4x::Body b;
    b.id = body_id;
    b.name = "Test Planet";
    b.type = nebula4x::BodyType::Planet;
    b.system_id = sys_id;
    b.orbit_radius_mkm = 0.0;
    b.orbit_period_days = 1.0;
    b.orbit_phase_radians = 0.0;
    b.mineral_deposits = {{"Duranium", 15.0}};
    s.bodies[b.id] = b;
  }

  const nebula4x::Id faction_id = 3;
  {
    nebula4x::Faction f;
    f.id = faction_id;
    f.name = "Test Faction";
    f.control = nebula4x::FactionControl::Player;
    s.factions[f.id] = f;
  }

  const nebula4x::Id colony_id = 4;
  {
    nebula4x::Colony c;
    c.id = colony_id;
    c.name = "Test Colony";
    c.faction_id = faction_id;
    c.body_id = body_id;
    c.population_millions = 0.0;
    c.installations = {{"automated_mine", 1}};
    s.colonies[c.id] = c;
  }

  s.next_id = 100;

  sim.load_game(std::move(s));

  {
    const auto* body = nebula4x::find_ptr(sim.state().bodies, body_id);
    N4X_ASSERT(body);
    N4X_ASSERT(std::abs(body->mineral_deposits.at("Duranium") - 15.0) < 1e-9);
  }

  sim.advance_days(1);
  {
    const auto* body = nebula4x::find_ptr(sim.state().bodies, body_id);
    const auto* col = nebula4x::find_ptr(sim.state().colonies, colony_id);
    N4X_ASSERT(body && col);
    N4X_ASSERT(std::abs(col->minerals.at("Duranium") - 10.0) < 1e-9);
    N4X_ASSERT(std::abs(body->mineral_deposits.at("Duranium") - 5.0) < 1e-9);
  }

  sim.advance_days(1);
  {
    const auto* body = nebula4x::find_ptr(sim.state().bodies, body_id);
    const auto* col = nebula4x::find_ptr(sim.state().colonies, colony_id);
    N4X_ASSERT(body && col);
    N4X_ASSERT(std::abs(col->minerals.at("Duranium") - 15.0) < 1e-9);
    N4X_ASSERT(std::abs(body->mineral_deposits.at("Duranium")) < 1e-9);
  }

  // Depletion should have generated a warning event.
  N4X_ASSERT(sim.state().events.size() == 1);
  {
    const auto& ev = sim.state().events.back();
    N4X_ASSERT(ev.level == nebula4x::EventLevel::Warn);
    N4X_ASSERT(ev.category == nebula4x::EventCategory::Construction);
    N4X_ASSERT(ev.system_id == sys_id);
    N4X_ASSERT(ev.colony_id == colony_id);
    N4X_ASSERT(ev.message.find("Mineral deposit depleted") != std::string::npos);
  }

  return 0;
}
