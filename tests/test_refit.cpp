#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>

#include "nebula4x/core/simulation.h"

#define N4X_ASSERT(cond, msg)                                                       \
  do {                                                                              \
    if (!(cond)) {                                                                  \
      std::cerr << "ASSERT FAIL: " << (msg) << " (" << __FILE__ << ":" << __LINE__ \
                << ")\n";                                                           \
      return 1;                                                                     \
    }                                                                               \
  } while (0)

namespace {

// Convenience: get mineral amount (0 if missing).
double get_mineral(const nebula4x::Colony& c, const std::string& mineral) {
  auto it = c.minerals.find(mineral);
  return (it == c.minerals.end()) ? 0.0 : it->second;
}

// Convenience: get cargo amount (0 if missing).
double get_cargo(const nebula4x::Ship& s, const std::string& mineral) {
  auto it = s.cargo.find(mineral);
  return (it == s.cargo.end()) ? 0.0 : it->second;
}

}  // namespace

int test_refit() {
  using namespace nebula4x;

  ContentDB content;

  // Shipyard with clear per-ton cost and a nice round build rate.
  InstallationDef yard;
  yard.id = "shipyard";
  yard.name = "Shipyard";
  yard.build_rate_tons_per_day = 100.0;
  yard.build_costs_per_ton["Duranium"] = 1.0;
  content.installations[yard.id] = yard;

  ShipDesign old_d;
  old_d.id = "old";
  old_d.name = "Old Design";
  old_d.role = ShipRole::Freighter;
  old_d.mass_tons = 100.0;
  old_d.max_hp = 100.0;
  old_d.speed_km_s = 0.0;
  old_d.cargo_tons = 200.0;
  content.designs[old_d.id] = old_d;

  ShipDesign new_d;
  new_d.id = "new";
  new_d.name = "New Design";
  new_d.role = ShipRole::Freighter;
  new_d.mass_tons = 100.0;
  new_d.max_hp = 200.0;
  new_d.speed_km_s = 0.0;
  new_d.cargo_tons = 50.0;
  content.designs[new_d.id] = new_d;

  // A larger design used to validate that a stalled refit does not block
  // subsequent shipyard orders.
  ShipDesign build_d;
  build_d.id = "build";
  build_d.name = "Build Design";
  build_d.role = ShipRole::Freighter;
  build_d.mass_tons = 200.0;
  build_d.max_hp = 100.0;
  build_d.speed_km_s = 0.0;
  build_d.cargo_tons = 0.0;
  content.designs[build_d.id] = build_d;

  SimConfig cfg;
  cfg.ship_refit_tons_multiplier = 0.5;  // 100t ship => 50t of work.

  Simulation sim(content, cfg);

  GameState st;
  st.save_version = GameState{}.save_version;

  // Faction.
  Faction f;
  f.id = 1;
  f.name = "Player";
  f.control = FactionControl::Player;
  st.factions[f.id] = f;

  // One system, one body at origin (so docking is trivial).
  StarSystem sys;
  sys.id = 1;
  sys.name = "Sys";
  sys.galaxy_pos = Vec2{0.0, 0.0};
  st.systems[sys.id] = sys;

  Body body;
  body.id = 10;
  body.name = "ColonyBody";
  body.system_id = sys.id;
  body.orbit_radius_mkm = 0.0;
  body.orbit_period_days = 1.0;
  body.orbit_phase_radians = 0.0;
  st.bodies[body.id] = body;

  Colony c;
  c.id = 20;
  c.name = "Colony";
  c.faction_id = f.id;
  c.body_id = body.id;
  c.installations["shipyard"] = 1;
  c.minerals["Duranium"] = 1000.0;
  st.colonies[c.id] = c;

  Ship sh;
  sh.id = 100;
  sh.name = "Ship-1";
  sh.faction_id = f.id;
  sh.design_id = old_d.id;
  sh.system_id = sys.id;
  sh.position_mkm = Vec2{0.0, 0.0};  // Docked.
  sh.cargo["Duranium"] = 120.0;      // Over the new capacity.
  st.ships[sh.id] = sh;

  sim.load_game(st);

  // Queue refit.
  std::string err;
  const bool ok = sim.enqueue_refit(c.id, sh.id, new_d.id, &err);
  N4X_ASSERT(ok, std::string("enqueue_refit succeeded: ") + err);

  // One day is enough (50 tons / 100 tpd).
  sim.advance_days(1);

  const Colony& c_after = sim.state().colonies.at(c.id);
  const Ship& sh_after = sim.state().ships.at(sh.id);

  N4X_ASSERT(sh_after.design_id == new_d.id, "ship design_id updated after refit");
  N4X_ASSERT(std::abs(sh_after.hp - new_d.max_hp) < 1e-6, "ship fully repaired to new design max hp");
  N4X_ASSERT(std::abs(get_cargo(sh_after, "Duranium") - 50.0) < 1e-6, "ship cargo clamped to new cargo capacity");

  // Mineral accounting:
  // - 50 tons of work @ 1 Duranium/ton => 50 Duranium consumed by the shipyard.
  // - 70 Duranium moved from ship cargo back to colony stockpile.
  // Start: colony 1000, ship 120 => total 1120.
  // End: colony should be 1000 - 50 + 70 = 1020, ship should be 50 => total 1070.
  N4X_ASSERT(std::abs(get_mineral(c_after, "Duranium") - 1020.0) < 1e-6, "colony mineral reflects cost + cargo return");
  N4X_ASSERT(std::abs(get_mineral(c_after, "Duranium") + get_cargo(sh_after, "Duranium") - 1070.0) < 1e-6,
             "total minerals conserved minus shipyard cost");

  N4X_ASSERT(c_after.shipyard_queue.empty(), "shipyard queue empty after completion");

  // --- Scheduled refit: queue a refit even when the ship is not docked.
  // The shipyard should skip the stalled refit and still make progress on later
  // build orders.
  {
    GameState st2;
    st2.save_version = GameState{}.save_version;

    st2.factions[f.id] = f;
    st2.systems[sys.id] = sys;
    st2.bodies[body.id] = body;

    Colony c2 = c;
    c2.id = 21;
    c2.name = "Colony2";
    c2.shipyard_queue.clear();
    c2.minerals["Duranium"] = 1000.0;
    st2.colonies[c2.id] = c2;

    Ship sh2 = sh;
    sh2.id = 101;
    sh2.name = "Ship-2";
    sh2.design_id = old_d.id;
    sh2.position_mkm = Vec2{10.0, 0.0};  // Not docked.
    sh2.cargo.clear();
    st2.ships[sh2.id] = sh2;

    sim.load_game(st2);

    std::string err2;
    const bool ok_refit = sim.enqueue_refit(c2.id, sh2.id, new_d.id, &err2);
    N4X_ASSERT(ok_refit, std::string("scheduled enqueue_refit succeeded: ") + err2);

    const bool ok_build = sim.enqueue_build(c2.id, build_d.id);
    N4X_ASSERT(ok_build, "enqueue_build succeeded");

    const double refit_remaining_day0 = sim.state().colonies.at(c2.id).shipyard_queue.at(0).tons_remaining;

    // Day 1: build order should progress, stalled refit should not.
    sim.advance_days(1);
    const Colony& c2_day1 = sim.state().colonies.at(c2.id);
    N4X_ASSERT(c2_day1.shipyard_queue.size() == 2, "shipyard queue still contains refit + build");
    N4X_ASSERT(c2_day1.shipyard_queue[0].is_refit(), "refit order remains queued");
    N4X_ASSERT(std::abs(c2_day1.shipyard_queue[0].tons_remaining - refit_remaining_day0) < 1e-6,
               "stalled refit should not progress while ship is undocked");
    N4X_ASSERT(std::abs(c2_day1.shipyard_queue[1].tons_remaining - 100.0) < 1e-6,
               "build order progressed despite stalled refit ahead of it");

    // Instantly dock the ship.
    sim.state().ships.at(sh2.id).position_mkm = Vec2{0.0, 0.0};

    // Day 2: refit should complete, build order should remain at 100 tons.
    sim.advance_days(1);
    const Ship& sh2_after = sim.state().ships.at(sh2.id);
    N4X_ASSERT(sh2_after.design_id == new_d.id, "scheduled refit completes once ship docks");

    const Colony& c2_day2 = sim.state().colonies.at(c2.id);
    N4X_ASSERT(c2_day2.shipyard_queue.size() == 1, "refit order removed after completion");
    N4X_ASSERT(c2_day2.shipyard_queue[0].design_id == build_d.id, "remaining shipyard order is the build");
    N4X_ASSERT(std::abs(c2_day2.shipyard_queue[0].tons_remaining - 100.0) < 1e-6,
               "build order did not receive leftover capacity from the refit");
  }

  return 0;
}
