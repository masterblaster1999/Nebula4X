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

}  // namespace

int test_auto_freight() {
  using namespace nebula4x;

  // Minimal content: shipyard def with a clear per-ton cost.
  ContentDB content;

  InstallationDef yard;
  yard.id = "shipyard";
  yard.name = "Shipyard";
  yard.build_rate_tons_per_day = 100.0;
  yard.build_costs_per_ton["Duranium"] = 1.0;
  content.installations[yard.id] = yard;

  // Target design for the shipyard queue.
  ShipDesign target;
  target.id = "target";
  target.name = "Target";
  target.role = ShipRole::Combatant;
  target.mass_tons = 100.0;
  target.max_hp = 100.0;
  target.speed_km_s = 0.0;
  content.designs[target.id] = target;

  // Freighter design with cargo.
  ShipDesign freighter;
  freighter.id = "freighter";
  freighter.name = "Freighter";
  freighter.role = ShipRole::Freighter;
  freighter.mass_tons = 100.0;
  freighter.max_hp = 100.0;
  freighter.speed_km_s = 100.0;
  freighter.cargo_tons = 500.0;
  content.designs[freighter.id] = freighter;

  SimConfig cfg;
  cfg.auto_freight_min_transfer_tons = 1.0;
  cfg.auto_freight_max_take_fraction_of_surplus = 1.0;

  Simulation sim(content, cfg);

  GameState st;
  st.save_version = GameState{}.save_version;

  // Faction.
  Faction f;
  f.id = 1;
  f.name = "Player";
  f.control = FactionControl::Player;
  st.factions[f.id] = f;

  // One system, two bodies at origin (zero orbit) so loading/unloading can complete immediately.
  StarSystem sys;
  sys.id = 1;
  sys.name = "Sol";
  sys.galaxy_pos = Vec2{0.0, 0.0};
  st.systems[sys.id] = sys;

  Body src_body;
  src_body.id = 10;
  src_body.name = "Source";
  src_body.system_id = sys.id;
  src_body.orbit_radius_mkm = 0.0;
  src_body.orbit_period_days = 1.0;
  src_body.orbit_phase_radians = 0.0;
  st.bodies[src_body.id] = src_body;

  Body dst_body;
  dst_body.id = 11;
  dst_body.name = "Dest";
  dst_body.system_id = sys.id;
  dst_body.orbit_radius_mkm = 0.0;
  dst_body.orbit_period_days = 1.0;
  dst_body.orbit_phase_radians = 0.0;
  st.bodies[dst_body.id] = dst_body;

  // Source colony has minerals.
  Colony src;
  src.id = 20;
  src.name = "Earth";
  src.faction_id = f.id;
  src.body_id = src_body.id;
  src.population_millions = 1000.0;
  src.minerals["Duranium"] = 1000.0;
  st.colonies[src.id] = src;

  // Dest colony has a shipyard queue but no minerals.
  Colony dst;
  dst.id = 21;
  dst.name = "Mars";
  dst.faction_id = f.id;
  dst.body_id = dst_body.id;
  dst.population_millions = 100.0;
  dst.installations["shipyard"] = 1;
  dst.shipyard_queue.push_back(BuildOrder{.design_id = "target", .tons_remaining = 100.0});
  st.colonies[dst.id] = dst;

  // Idle auto-freight ship.
  Ship sh;
  sh.id = 100;
  sh.name = "Cargo-1";
  sh.faction_id = f.id;
  sh.design_id = freighter.id;
  sh.system_id = sys.id;
  sh.position_mkm = Vec2{0.0, 0.0};
  sh.auto_freight = true;
  st.ships[sh.id] = sh;

  sim.load_game(st);

  const double src_before = get_mineral(sim.state().colonies.at(src.id), "Duranium");
  const double dst_before = get_mineral(sim.state().colonies.at(dst.id), "Duranium");
  N4X_ASSERT(std::abs(src_before - 1000.0) < 1e-6, "source starts with 1000 Duranium");
  N4X_ASSERT(std::abs(dst_before - 0.0) < 1e-6, "destination starts with 0 Duranium");

  // Run one day: shipyard stalls, then auto-freight should schedule + complete the haul in the same day.
  sim.advance_days(1);

  const double src_after = get_mineral(sim.state().colonies.at(src.id), "Duranium");
  const double dst_after = get_mineral(sim.state().colonies.at(dst.id), "Duranium");

  N4X_ASSERT(dst_after > 0.0, "destination received Duranium via auto-freight");
  N4X_ASSERT(src_after < 1000.0, "source exported some Duranium via auto-freight");

  // Conservation check (no mines/industry in this test).
  N4X_ASSERT(std::abs((src_after + dst_after) - 1000.0) < 1e-6,
             "total Duranium conserved between colonies");

  // Since both bodies are at the same position, loading and unloading should complete in the same day.
  const Ship& tsh = sim.state().ships.at(sh.id);
  auto it = tsh.cargo.find("Duranium");
  const double cargo_after = (it == tsh.cargo.end()) ? 0.0 : it->second;
  N4X_ASSERT(cargo_after < 1e-6, "ship cargo is empty after same-day delivery");


  // 2) Manual mineral reserves should cap exports.
  {
    Simulation sim2(content, cfg);

    GameState st2 = st;
    st2.colonies[src.id].mineral_reserves["Duranium"] = 950.0;

    sim2.load_game(st2);

    sim2.advance_days(1);

    const double src_after2 = get_mineral(sim2.state().colonies.at(src.id), "Duranium");
    const double dst_after2 = get_mineral(sim2.state().colonies.at(dst.id), "Duranium");

    N4X_ASSERT(std::abs(src_after2 - 950.0) < 1e-6, "source kept reserve (export capped)");
    N4X_ASSERT(std::abs(dst_after2 - 50.0) < 1e-6, "destination received only surplus above reserve");

    N4X_ASSERT(std::abs((src_after2 + dst_after2) - 1000.0) < 1e-6,
               "total Duranium conserved with reserves");
  }

  // 3) Multi-mineral bundling: if a destination colony is missing multiple minerals,
  // auto-freight should (by default) bundle them into a single trip when it can.
  {
    ContentDB content3 = content;
    content3.installations[yard.id].build_costs_per_ton["Corbomite"] = 1.0;

    Simulation sim3(content3, cfg);

    GameState st3 = st;
    st3.colonies[src.id].minerals["Corbomite"] = 1000.0;

    sim3.load_game(st3);

    sim3.advance_days(1);

    const double src_dur = get_mineral(sim3.state().colonies.at(src.id), "Duranium");
    const double dst_dur = get_mineral(sim3.state().colonies.at(dst.id), "Duranium");
    const double src_cor = get_mineral(sim3.state().colonies.at(src.id), "Corbomite");
    const double dst_cor = get_mineral(sim3.state().colonies.at(dst.id), "Corbomite");

    N4X_ASSERT(std::abs(dst_dur - 100.0) < 1e-6, "destination received bundled Duranium");
    N4X_ASSERT(std::abs(dst_cor - 100.0) < 1e-6, "destination received bundled Corbomite");
    N4X_ASSERT(std::abs(src_dur - 900.0) < 1e-6, "source exported bundled Duranium");
    N4X_ASSERT(std::abs(src_cor - 900.0) < 1e-6, "source exported bundled Corbomite");

    const Ship& tsh3 = sim3.state().ships.at(sh.id);
    const double cargo_dur = tsh3.cargo.count("Duranium") ? tsh3.cargo.at("Duranium") : 0.0;
    const double cargo_cor = tsh3.cargo.count("Corbomite") ? tsh3.cargo.at("Corbomite") : 0.0;
    N4X_ASSERT(cargo_dur < 1e-6 && cargo_cor < 1e-6, "ship cargo is empty after bundled same-day delivery");
  }

  // 4) Bundling can be disabled via config, falling back to one-mineral-per-trip.
  {
    ContentDB content4 = content;
    content4.installations[yard.id].build_costs_per_ton["Corbomite"] = 1.0;

    SimConfig cfg4 = cfg;
    cfg4.auto_freight_multi_mineral = false;

    Simulation sim4(content4, cfg4);

    GameState st4 = st;
    st4.colonies[src.id].minerals["Corbomite"] = 1000.0;

    sim4.load_game(st4);

    // Day 1: only the first mineral in deterministic priority order should arrive.
    sim4.advance_days(1);
    const double dst_dur1 = get_mineral(sim4.state().colonies.at(dst.id), "Duranium");
    const double dst_cor1 = get_mineral(sim4.state().colonies.at(dst.id), "Corbomite");
    N4X_ASSERT(std::abs(dst_cor1 - 100.0) < 1e-6, "first trip delivered Corbomite");
    N4X_ASSERT(std::abs(dst_dur1 - 0.0) < 1e-6, "first trip did not deliver Duranium");

    // Day 2: the remaining mineral should be delivered.
    sim4.advance_days(1);
    const double dst_dur2 = get_mineral(sim4.state().colonies.at(dst.id), "Duranium");
    const double dst_cor2 = get_mineral(sim4.state().colonies.at(dst.id), "Corbomite");
    N4X_ASSERT(std::abs(dst_cor2 - 100.0) < 1e-6, "second day preserved Corbomite delivery");
    N4X_ASSERT(std::abs(dst_dur2 - 100.0) < 1e-6, "second day delivered Duranium");
  }

  return 0;
}
