#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "nebula4x/core/simulation.h"
#include "nebula4x/core/serialization.h"

#define N4X_ASSERT(expr)                                                                            \
  do {                                                                                              \
    if (!(expr)) {                                                                                  \
      std::cerr << "ASSERT failed: " #expr " (" << __FILE__ << ":" << __LINE__ << ")\n";           \
      return 1;                                                                                     \
    }                                                                                               \
  } while (0)

namespace {

nebula4x::Id find_colony_id(const nebula4x::GameState& st, const std::string& name) {
  for (const auto& [cid, c] : st.colonies) {
    if (c.name == name) return cid;
  }
  return nebula4x::kInvalidId;
}

nebula4x::Id find_ship_id(const nebula4x::GameState& st, const std::string& name) {
  for (const auto& [sid, sh] : st.ships) {
    if (sh.name == name) return sid;
  }
  return nebula4x::kInvalidId;
}

nebula4x::Id find_system_id(const nebula4x::GameState& st, const std::string& name) {
  for (const auto& [sid, sys] : st.systems) {
    if (sys.name == name) return sid;
  }
  return nebula4x::kInvalidId;
}

nebula4x::Id find_faction_id(const nebula4x::GameState& st, const std::string& name) {
  for (const auto& [fid, f] : st.factions) {
    if (f.name == name) return fid;
  }
  return nebula4x::kInvalidId;
}

} // namespace

int test_simulation() {
  // --- mineral production sanity check ---
  {
    nebula4x::ContentDB content;

    nebula4x::InstallationDef mine;
    mine.id = "automated_mine";
    mine.name = "Automated Mine";
    mine.produces_per_day = {{"Duranium", 1.0}};
    content.installations[mine.id] = mine;

    nebula4x::InstallationDef yard;
    yard.id = "shipyard";
    yard.name = "Shipyard";
    yard.build_rate_tons_per_day = 50;
    content.installations[yard.id] = yard;

    nebula4x::ShipDesign d;
    d.id = "freighter_alpha";
    d.name = "Freighter Alpha";
    d.mass_tons = 100;
    d.speed_km_s = 10;
    content.designs[d.id] = d;

    nebula4x::Simulation sim(std::move(content), nebula4x::SimConfig{});

    const auto earth_id = find_colony_id(sim.state(), "Earth");
    N4X_ASSERT(earth_id != nebula4x::kInvalidId);

    const double before = sim.state().colonies[earth_id].minerals["Duranium"];
    sim.advance_days(2);
    const double after = sim.state().colonies[earth_id].minerals["Duranium"];
    N4X_ASSERT(after > before);
  }

  // --- shipyard build costs sanity check ---
  // When build_costs_per_ton are configured for the shipyard, advancing time with an
  // active build order should consume minerals.
  {
    nebula4x::ContentDB content;

    nebula4x::InstallationDef yard;
    yard.id = "shipyard";
    yard.name = "Shipyard";
    yard.build_rate_tons_per_day = 50;
    yard.build_costs_per_ton = {{"Duranium", 1.0}}; // 1 mineral per ton
    content.installations[yard.id] = yard;

    nebula4x::InstallationDef mine;
    mine.id = "automated_mine";
    mine.name = "Automated Mine";
    mine.produces_per_day = {{"Duranium", 0.0}}; // keep minerals stable for the check
    content.installations[mine.id] = mine;

    nebula4x::ShipDesign d;
    d.id = "freighter_alpha";
    d.name = "Freighter Alpha";
    d.mass_tons = 100;
    d.speed_km_s = 10;
    content.designs[d.id] = d;

    nebula4x::Simulation sim(std::move(content), nebula4x::SimConfig{});
    const auto earth_id = find_colony_id(sim.state(), "Earth");
    N4X_ASSERT(earth_id != nebula4x::kInvalidId);

    const double before = sim.state().colonies[earth_id].minerals["Duranium"];
    N4X_ASSERT(sim.enqueue_build(earth_id, "freighter_alpha"));
    sim.advance_days(1);
    const double after = sim.state().colonies[earth_id].minerals["Duranium"];
    N4X_ASSERT(after < before);
  }

  // --- colony construction queue sanity check ---
  // Enqueuing an installation build should consume minerals and increase
  // installation count once enough construction points are available.
  {
    nebula4x::ContentDB content;

    nebula4x::InstallationDef mine;
    mine.id = "automated_mine";
    mine.name = "Automated Mine";
    mine.produces_per_day = {{"Duranium", 0.0}}; // avoid production influencing the check
    mine.construction_cost = 50.0;
    mine.build_costs = {{"Duranium", 100.0}};
    content.installations[mine.id] = mine;

    nebula4x::InstallationDef yard;
    yard.id = "shipyard";
    yard.name = "Shipyard";
    yard.build_rate_tons_per_day = 0.0;
    content.installations[yard.id] = yard;

    nebula4x::Simulation sim(std::move(content), nebula4x::SimConfig{});

    const auto earth_id = find_colony_id(sim.state(), "Earth");
    N4X_ASSERT(earth_id != nebula4x::kInvalidId);
    const auto& before_col = sim.state().colonies[earth_id];

    const int mines_before = before_col.installations.count("automated_mine") ? before_col.installations.at("automated_mine") : 0;
    const double dur_before = before_col.minerals.count("Duranium") ? before_col.minerals.at("Duranium") : 0.0;

    N4X_ASSERT(sim.enqueue_installation_build(earth_id, "automated_mine", 1));
    sim.advance_days(1);

    const auto& after_col = sim.state().colonies[earth_id];
    const int mines_after = after_col.installations.count("automated_mine") ? after_col.installations.at("automated_mine") : 0;
    const double dur_after = after_col.minerals.count("Duranium") ? after_col.minerals.at("Duranium") : 0.0;

    N4X_ASSERT(mines_after == mines_before + 1);
    N4X_ASSERT(dur_after < dur_before);

    // Event log should record the completion.
    bool saw_event = false;
    for (const auto& ev : sim.state().events) {
      if (ev.message.find("Constructed Automated Mine") != std::string::npos) {
        saw_event = true;
        break;
      }
    }
    N4X_ASSERT(saw_event);
  }

  
// --- construction queue should not be blocked by stalled front order ---
// If the first queued build cannot start due to missing minerals, later orders
// should still be able to progress.
{
  nebula4x::ContentDB content;

  // Define two installations that are already unlocked in the default scenario.
  nebula4x::InstallationDef expensive;
  expensive.id = "research_lab";
  expensive.name = "Research Lab";
  expensive.construction_cost = 20.0;
  expensive.build_costs = {{"Duranium", 1.0e9}}; // unaffordable -> stall
  content.installations[expensive.id] = expensive;

  nebula4x::InstallationDef cheap;
  cheap.id = "sensor_station";
  cheap.name = "Sensor Station";
  cheap.construction_cost = 5.0;
  // No mineral costs -> should be buildable even when the first order stalls.
  content.installations[cheap.id] = cheap;

  nebula4x::Simulation sim(std::move(content), nebula4x::SimConfig{});
  const auto earth_id = find_colony_id(sim.state(), "Earth");
  N4X_ASSERT(earth_id != nebula4x::kInvalidId);

  const auto& before = sim.state().colonies.at(earth_id);
  const int sensors_before =
      before.installations.count("sensor_station") ? before.installations.at("sensor_station") : 0;

  N4X_ASSERT(sim.enqueue_installation_build(earth_id, "research_lab", 1));
  N4X_ASSERT(sim.enqueue_installation_build(earth_id, "sensor_station", 1));

  sim.advance_days(1);

  const auto& after = sim.state().colonies.at(earth_id);
  const int sensors_after =
      after.installations.count("sensor_station") ? after.installations.at("sensor_station") : 0;

  N4X_ASSERT(sensors_after == sensors_before + 1);
}

// --- deleting a construction order refunds already-paid minerals for the current unit ---
{
  nebula4x::ContentDB content;

  nebula4x::InstallationDef fac;
  fac.id = "construction_factory";
  fac.name = "Construction Factory";
  fac.construction_cost = 200.0;           // > Earth daily CP baseline, so it won't finish in a day.
  fac.build_costs = {{"Duranium", 100.0}}; // paid up-front per unit
  content.installations[fac.id] = fac;

  nebula4x::Simulation sim(std::move(content), nebula4x::SimConfig{});
  const auto earth_id = find_colony_id(sim.state(), "Earth");
  N4X_ASSERT(earth_id != nebula4x::kInvalidId);

  const double dur_before = sim.state().colonies.at(earth_id).minerals.at("Duranium");

  N4X_ASSERT(sim.enqueue_installation_build(earth_id, "construction_factory", 1));
  sim.advance_days(1);

  const double dur_after_pay = sim.state().colonies.at(earth_id).minerals.at("Duranium");
  N4X_ASSERT(dur_after_pay < dur_before);

  N4X_ASSERT(sim.delete_construction_order(earth_id, 0, true));
  const double dur_after_refund = sim.state().colonies.at(earth_id).minerals.at("Duranium");

  N4X_ASSERT(std::abs(dur_after_refund - dur_before) < 1e-6);
}

// --- shipyard queue editing helpers ---
{
  nebula4x::ContentDB content;

  nebula4x::InstallationDef yard;
  yard.id = "shipyard";
  yard.name = "Shipyard";
  yard.build_rate_tons_per_day = 50;
  yard.build_costs_per_ton = {{"Duranium", 1.0}};
  content.installations[yard.id] = yard;

  nebula4x::ShipDesign a;
  a.id = "a";
  a.name = "A";
  a.mass_tons = 100;
  a.speed_km_s = 0;
  content.designs[a.id] = a;

  nebula4x::ShipDesign b;
  b.id = "b";
  b.name = "B";
  b.mass_tons = 100;
  b.speed_km_s = 0;
  content.designs[b.id] = b;

  nebula4x::Simulation sim(std::move(content), nebula4x::SimConfig{});
  const auto earth_id = find_colony_id(sim.state(), "Earth");
  N4X_ASSERT(earth_id != nebula4x::kInvalidId);

  N4X_ASSERT(sim.enqueue_build(earth_id, "a"));
  N4X_ASSERT(sim.enqueue_build(earth_id, "b"));

  auto& q = sim.state().colonies.at(earth_id).shipyard_queue;
  N4X_ASSERT(q.size() == 2);
  N4X_ASSERT(q[0].design_id == "a");
  N4X_ASSERT(q[1].design_id == "b");

  N4X_ASSERT(sim.move_shipyard_order(earth_id, 1, 0));
  N4X_ASSERT(q[0].design_id == "b");
  N4X_ASSERT(q[1].design_id == "a");

  N4X_ASSERT(sim.delete_shipyard_order(earth_id, 0));
  N4X_ASSERT(q.size() == 1);
  N4X_ASSERT(q[0].design_id == "a");
}

// --- cargo transfer sanity check ---
  // Load minerals from Earth onto a freighter, then unload to Mars Outpost.
  {
    nebula4x::ContentDB content;

    // Installations referenced by the default scenario.
    nebula4x::InstallationDef mine;
    mine.id = "automated_mine";
    mine.name = "Automated Mine";
    mine.produces_per_day = {{"Duranium", 0.0}};
    content.installations[mine.id] = mine;

    nebula4x::InstallationDef yard;
    yard.id = "shipyard";
    yard.name = "Shipyard";
    yard.build_rate_tons_per_day = 0.0;
    content.installations[yard.id] = yard;

    // Designs used by the default scenario.
    auto make_min_design = [](const std::string& id) {
      nebula4x::ShipDesign d;
      d.id = id;
      d.name = id;
      d.max_hp = 10.0;
      d.speed_km_s = 0.0;
      d.sensor_range_mkm = 0.0;
      return d;
    };

    nebula4x::ShipDesign freighter;
    freighter.id = "freighter_alpha";
    freighter.name = "Freighter Alpha";
    freighter.max_hp = 10.0;
    freighter.cargo_tons = 1000.0;
    freighter.speed_km_s = 100000.0; // fast enough to reach Mars quickly in the prototype
    content.designs[freighter.id] = freighter;

    content.designs["surveyor_beta"] = make_min_design("surveyor_beta");
    content.designs["escort_gamma"] = make_min_design("escort_gamma");
    content.designs["pirate_raider"] = make_min_design("pirate_raider");

    nebula4x::Simulation sim(std::move(content), nebula4x::SimConfig{});

    const auto earth_id = find_colony_id(sim.state(), "Earth");
    const auto mars_id = find_colony_id(sim.state(), "Mars Outpost");
    N4X_ASSERT(earth_id != nebula4x::kInvalidId);
    N4X_ASSERT(mars_id != nebula4x::kInvalidId);

    const auto freighter_id = find_ship_id(sim.state(), "Freighter Alpha");
    N4X_ASSERT(freighter_id != nebula4x::kInvalidId);

    const double earth_before = sim.state().colonies[earth_id].minerals["Duranium"];
    const double mars_before = sim.state().colonies[mars_id].minerals["Duranium"];

    N4X_ASSERT(sim.issue_load_mineral(freighter_id, earth_id, "Duranium", 100.0));
    N4X_ASSERT(sim.issue_unload_mineral(freighter_id, mars_id, "Duranium", 100.0));

    sim.advance_days(2);

    const double earth_after = sim.state().colonies[earth_id].minerals["Duranium"];
    const double mars_after = sim.state().colonies[mars_id].minerals["Duranium"];

    const auto* sh_after = nebula4x::find_ptr(sim.state().ships, freighter_id);
    N4X_ASSERT(sh_after);
    const double ship_cargo_after = sh_after->cargo.count("Duranium") ? sh_after->cargo.at("Duranium") : 0.0;

    N4X_ASSERT(earth_after < earth_before);
    N4X_ASSERT(mars_after > mars_before);
    N4X_ASSERT(ship_cargo_after < 1.0); // should have unloaded most/all
  }

  // --- cargo waiting + docking tolerance sanity check ---
  // A ship that is "in orbit" should be able to keep transferring cargo even though
  // the planet position updates day-to-day (i.e. no requirement to match a body's
  // exact coordinates every tick).
  {
    nebula4x::ContentDB content;

    // Scenario installs 5 automated mines on Mars. Produce Neutronium so we can
    // validate a multi-day load order.
    nebula4x::InstallationDef mine;
    mine.id = "automated_mine";
    mine.name = "Automated Mine";
    mine.produces_per_day = {{"Neutronium", 1.0}}; // 5 mines -> 5 / day
    content.installations[mine.id] = mine;

    nebula4x::InstallationDef yard;
    yard.id = "shipyard";
    yard.name = "Shipyard";
    yard.build_rate_tons_per_day = 0.0;
    content.installations[yard.id] = yard;

    // Minimal designs to satisfy scenario ships.
    auto make_min_design = [](const std::string& id) {
      nebula4x::ShipDesign d;
      d.id = id;
      d.name = id;
      d.max_hp = 10.0;
      d.speed_km_s = 0.0;
      d.sensor_range_mkm = 0.0;
      return d;
    };

    nebula4x::ShipDesign freighter;
    freighter.id = "freighter_alpha";
    freighter.name = "Freighter Alpha";
    freighter.max_hp = 10.0;
    freighter.cargo_tons = 1000.0;
    freighter.speed_km_s = 0.0; // intentionally 0: must rely on docking tolerance
    content.designs[freighter.id] = freighter;

    content.designs["surveyor_beta"] = make_min_design("surveyor_beta");
    content.designs["escort_gamma"] = make_min_design("escort_gamma");
    content.designs["pirate_raider"] = make_min_design("pirate_raider");

    nebula4x::Simulation sim(std::move(content), nebula4x::SimConfig{});

    const auto mars_id = find_colony_id(sim.state(), "Mars Outpost");
    N4X_ASSERT(mars_id != nebula4x::kInvalidId);

    const auto freighter_id = find_ship_id(sim.state(), "Freighter Alpha");
    N4X_ASSERT(freighter_id != nebula4x::kInvalidId);

    // Start "docked" at Mars so we can test multi-day transfers.
    auto& st = sim.state();
    auto& mars = st.colonies[mars_id];
    mars.minerals["Neutronium"] = 0.0;

    auto* sh = nebula4x::find_ptr(st.ships, freighter_id);
    N4X_ASSERT(sh);
    sh->cargo.clear();

    const auto* body = nebula4x::find_ptr(st.bodies, mars.body_id);
    N4X_ASSERT(body);
    sh->position_mkm = body->position_mkm;

    N4X_ASSERT(sim.issue_load_mineral(freighter_id, mars_id, "Neutronium", 10.0));

    sim.advance_days(1);

    sh = nebula4x::find_ptr(sim.state().ships, freighter_id);
    N4X_ASSERT(sh);
    const double cargo_day1 = sh->cargo.count("Neutronium") ? sh->cargo.at("Neutronium") : 0.0;
    N4X_ASSERT(std::fabs(cargo_day1 - 5.0) < 1e-6);

    // Order should still be present with 5 tons remaining.
    auto oit = sim.state().ship_orders.find(freighter_id);
    N4X_ASSERT(oit != sim.state().ship_orders.end());
    N4X_ASSERT(!oit->second.queue.empty());
    N4X_ASSERT(std::holds_alternative<nebula4x::LoadMineral>(oit->second.queue.front()));
    const auto& ord = std::get<nebula4x::LoadMineral>(oit->second.queue.front());
    N4X_ASSERT(std::fabs(ord.tons - 5.0) < 1e-6);

    sim.advance_days(1);

    sh = nebula4x::find_ptr(sim.state().ships, freighter_id);
    N4X_ASSERT(sh);
    const double cargo_day2 = sh->cargo.count("Neutronium") ? sh->cargo.at("Neutronium") : 0.0;
    N4X_ASSERT(std::fabs(cargo_day2 - 10.0) < 1e-6);

    // Order should be complete.
    oit = sim.state().ship_orders.find(freighter_id);
    N4X_ASSERT(oit != sim.state().ship_orders.end());
    N4X_ASSERT(oit->second.queue.empty());
  }

  // --- WaitDays order sanity check ---
  // WaitDays should delay subsequent orders by the requested number of days.
  {
    nebula4x::ContentDB content;

    // Minimal installations referenced by the default scenario.
    nebula4x::InstallationDef mine;
    mine.id = "automated_mine";
    mine.name = "Automated Mine";
    mine.produces_per_day = {{"Duranium", 0.0}};
    content.installations[mine.id] = mine;

    nebula4x::InstallationDef yard;
    yard.id = "shipyard";
    yard.name = "Shipyard";
    yard.build_rate_tons_per_day = 0.0;
    content.installations[yard.id] = yard;

    // Designs used by the scenario.
    auto make_min_design = [](const std::string& id, double speed_km_s) {
      nebula4x::ShipDesign d;
      d.id = id;
      d.name = id;
      d.max_hp = 10.0;
      d.speed_km_s = speed_km_s;
      d.sensor_range_mkm = 0.0;
      return d;
    };

    // Fast freighter so a move order can complete in a single day.
    content.designs["freighter_alpha"] = make_min_design("freighter_alpha", 100000.0);
    content.designs["surveyor_beta"] = make_min_design("surveyor_beta", 0.0);
    content.designs["escort_gamma"] = make_min_design("escort_gamma", 0.0);
    content.designs["pirate_raider"] = make_min_design("pirate_raider", 0.0);

    nebula4x::Simulation sim(std::move(content), nebula4x::SimConfig{});

    const auto freighter_id = find_ship_id(sim.state(), "Freighter Alpha");
    N4X_ASSERT(freighter_id != nebula4x::kInvalidId);

    const auto* sh0 = nebula4x::find_ptr(sim.state().ships, freighter_id);
    N4X_ASSERT(sh0);
    const nebula4x::Vec2 start_pos = sh0->position_mkm;

    N4X_ASSERT(sim.clear_orders(freighter_id));
    N4X_ASSERT(sim.issue_wait_days(freighter_id, 2));
    N4X_ASSERT(sim.issue_move_to_point(freighter_id, {0.0, 0.0}));

    // After 1 day: still waiting, no movement.
    sim.advance_days(1);
    const auto* sh1 = nebula4x::find_ptr(sim.state().ships, freighter_id);
    N4X_ASSERT(sh1);
    N4X_ASSERT(std::fabs(sh1->position_mkm.x - start_pos.x) < 1e-6);
    N4X_ASSERT(std::fabs(sh1->position_mkm.y - start_pos.y) < 1e-6);
    {
      const auto& q = sim.state().ship_orders.at(freighter_id).queue;
      N4X_ASSERT(!q.empty());
      N4X_ASSERT(std::holds_alternative<nebula4x::WaitDays>(q.front()));
      const auto& w = std::get<nebula4x::WaitDays>(q.front());
      N4X_ASSERT(w.days_remaining == 1);
    }

    // After 2 days: wait completes, move order is now at the front (but won't execute until next day).
    sim.advance_days(1);
    const auto* sh2 = nebula4x::find_ptr(sim.state().ships, freighter_id);
    N4X_ASSERT(sh2);
    N4X_ASSERT(std::fabs(sh2->position_mkm.x - start_pos.x) < 1e-6);
    N4X_ASSERT(std::fabs(sh2->position_mkm.y - start_pos.y) < 1e-6);
    {
      const auto& q = sim.state().ship_orders.at(freighter_id).queue;
      N4X_ASSERT(q.size() == 1);
      N4X_ASSERT(std::holds_alternative<nebula4x::MoveToPoint>(q.front()));
    }

    // After 3 days: move order should execute and complete.
    sim.advance_days(1);
    const auto* sh3 = nebula4x::find_ptr(sim.state().ships, freighter_id);
    N4X_ASSERT(sh3);
    N4X_ASSERT(std::fabs(sh3->position_mkm.x - 0.0) < 1e-6);
    N4X_ASSERT(std::fabs(sh3->position_mkm.y - 0.0) < 1e-6);
    N4X_ASSERT(sim.state().ship_orders.at(freighter_id).queue.empty());

    // Serialization round-trip for the order type.
    N4X_ASSERT(sim.issue_wait_days(freighter_id, 3));
    const std::string saved = nebula4x::serialize_game_to_json(sim.state());
    const auto loaded = nebula4x::deserialize_game_from_json(saved);
    const auto oit = loaded.ship_orders.find(freighter_id);
    N4X_ASSERT(oit != loaded.ship_orders.end());
    N4X_ASSERT(!oit->second.queue.empty());
    N4X_ASSERT(std::holds_alternative<nebula4x::WaitDays>(oit->second.queue.front()));
    const auto& w2 = std::get<nebula4x::WaitDays>(oit->second.queue.front());
    N4X_ASSERT(w2.days_remaining == 3);
  }

  // --- sensor detection sanity check ---
  {
    nebula4x::ContentDB content;

    // Minimal installations to satisfy scenario content references.
    nebula4x::InstallationDef mine;
    mine.id = "automated_mine";
    mine.name = "Automated Mine";
    mine.produces_per_day = {{"Duranium", 0.0}};
    content.installations[mine.id] = mine;

    nebula4x::InstallationDef yard;
    yard.id = "shipyard";
    yard.name = "Shipyard";
    yard.build_rate_tons_per_day = 0.0;
    content.installations[yard.id] = yard;

    // Designs used by the default scenario.
    auto make_design = [](const std::string& id, double sensor_range_mkm) {
      nebula4x::ShipDesign d;
      d.id = id;
      d.name = id;
      d.max_hp = 10.0;
      d.speed_km_s = 0.0;
      d.sensor_range_mkm = sensor_range_mkm;
      return d;
    };

    content.designs["freighter_alpha"] = make_design("freighter_alpha", 0.0);
    content.designs["surveyor_beta"] = make_design("surveyor_beta", 100.0);
    content.designs["escort_gamma"] = make_design("escort_gamma", 100.0);
    content.designs["pirate_raider"] = make_design("pirate_raider", 0.0);

    nebula4x::Simulation sim(std::move(content), nebula4x::SimConfig{});

    const auto terrans_id = find_faction_id(sim.state(), "Terran Union");
    const auto pirates_id = find_faction_id(sim.state(), "Pirate Raiders");
    N4X_ASSERT(terrans_id != nebula4x::kInvalidId);
    N4X_ASSERT(pirates_id != nebula4x::kInvalidId);

    const auto sol_id = find_system_id(sim.state(), "Sol");
    N4X_ASSERT(sol_id != nebula4x::kInvalidId);
    const auto* sol = nebula4x::find_ptr(sim.state().systems, sol_id);
    N4X_ASSERT(sol);

    // Use the first Terran ship as a reference point.
    nebula4x::Vec2 terran_pos{0.0, 0.0};
    bool found_terran_ship = false;
    for (nebula4x::Id sid : sol->ships) {
      const auto* sh = nebula4x::find_ptr(sim.state().ships, sid);
      if (sh && sh->faction_id == terrans_id) {
        terran_pos = sh->position_mkm;
        found_terran_ship = true;
        break;
      }
    }
    N4X_ASSERT(found_terran_ship);

    // Spawn a pirate ship within 100 mkm (detected), then move it out of range (not detected).
    const nebula4x::Id pirate_ship_id = nebula4x::allocate_id(sim.state());
    {
      nebula4x::Ship p;
      p.id = pirate_ship_id;
      p.name = "Test Raider";
      p.faction_id = pirates_id;
      p.system_id = sol_id;
      p.design_id = "pirate_raider";
      p.position_mkm = terran_pos + nebula4x::Vec2{0.0, 50.0};
      p.hp = 10.0;

      sim.state().ships[p.id] = p;
      sim.state().ship_orders[p.id] = nebula4x::ShipOrders{};
      sim.state().systems[sol_id].ships.push_back(p.id);
    }

    N4X_ASSERT(sim.is_ship_detected_by_faction(terrans_id, pirate_ship_id));

    sim.state().ships[pirate_ship_id].position_mkm = terran_pos + nebula4x::Vec2{0.0, 500.0};
    N4X_ASSERT(!sim.is_ship_detected_by_faction(terrans_id, pirate_ship_id));
  }

  // --- contact memory sanity check ---
  // If a hostile ship is detected, the viewer faction should store a contact snapshot
  // that remains for some time even after contact is lost.
  {
    nebula4x::ContentDB content;

    nebula4x::InstallationDef mine;
    mine.id = "automated_mine";
    mine.name = "Automated Mine";
    mine.produces_per_day = {{"Duranium", 0.0}};
    content.installations[mine.id] = mine;

    nebula4x::InstallationDef yard;
    yard.id = "shipyard";
    yard.name = "Shipyard";
    yard.build_rate_tons_per_day = 0.0;
    content.installations[yard.id] = yard;

    auto make_design = [](const std::string& id, double sensor_range_mkm) {
      nebula4x::ShipDesign d;
      d.id = id;
      d.name = id;
      d.max_hp = 10.0;
      d.speed_km_s = 0.0;
      d.sensor_range_mkm = sensor_range_mkm;
      return d;
    };

    content.designs["freighter_alpha"] = make_design("freighter_alpha", 0.0);
    content.designs["surveyor_beta"] = make_design("surveyor_beta", 100.0);
    content.designs["escort_gamma"] = make_design("escort_gamma", 100.0);
    content.designs["pirate_raider"] = make_design("pirate_raider", 0.0);

    nebula4x::Simulation sim(std::move(content), nebula4x::SimConfig{});

    const auto terrans = find_faction_id(sim.state(), "Terran Union");
    const auto pirates = find_faction_id(sim.state(), "Pirate Raiders");
    N4X_ASSERT(terrans != nebula4x::kInvalidId);
    N4X_ASSERT(pirates != nebula4x::kInvalidId);

    const auto sol = find_system_id(sim.state(), "Sol");
    N4X_ASSERT(sol != nebula4x::kInvalidId);
    const auto* sol_sys = nebula4x::find_ptr(sim.state().systems, sol);
    N4X_ASSERT(sol_sys);

    // Find a Terran ship (we'll use it for intercept checks).
    nebula4x::Id terran_ship = nebula4x::kInvalidId;
    nebula4x::Vec2 terran_pos{0.0, 0.0};
    for (nebula4x::Id sid : sol_sys->ships) {
      const auto* sh = nebula4x::find_ptr(sim.state().ships, sid);
      if (sh && sh->faction_id == terrans) {
        terran_pos = sh->position_mkm;
        terran_ship = sid;
        break;
      }
    }
    N4X_ASSERT(terran_ship != nebula4x::kInvalidId);

    // Spawn a pirate ship within detection range.
    const nebula4x::Id pirate_contact_id = nebula4x::allocate_id(sim.state());
    {
      nebula4x::Ship p;
      p.id = pirate_contact_id;
      p.name = "Contact Raider";
      p.faction_id = pirates;
      p.system_id = sol;
      p.design_id = "pirate_raider";
      p.position_mkm = terran_pos + nebula4x::Vec2{0.0, 50.0};
      p.hp = 10.0;
      sim.state().ships[p.id] = p;
      sim.state().ship_orders[p.id] = nebula4x::ShipOrders{};
      sim.state().systems[sol].ships.push_back(p.id);
    }

    sim.advance_days(1); // contacts tick
    N4X_ASSERT(sim.is_ship_detected_by_faction(terrans, pirate_contact_id));
    N4X_ASSERT(sim.state().factions[terrans].ship_contacts.count(pirate_contact_id) == 1);

    // Move out of range and advance; contact should remain but detection should be false.
    sim.state().ships[pirate_contact_id].position_mkm = terran_pos + nebula4x::Vec2{0.0, 500.0};
    sim.advance_days(1);
    N4X_ASSERT(!sim.is_ship_detected_by_faction(terrans, pirate_contact_id));
    N4X_ASSERT(sim.state().factions[terrans].ship_contacts.count(pirate_contact_id) == 1);

    const auto recent = sim.recent_contacts_in_system(terrans, sol, 30);
    N4X_ASSERT(!recent.empty());

    // --- contact-based intercept / attack order sanity check ---
    // A faction should be able to issue an AttackShip order against a target that is
    // not currently detected, as long as it has a stored contact snapshot in the same system.
    {
      const auto& contact = sim.state().factions[terrans].ship_contacts.at(pirate_contact_id);
      N4X_ASSERT(!sim.is_ship_detected_by_faction(terrans, pirate_contact_id));
      N4X_ASSERT(sim.issue_attack_ship(terran_ship, pirate_contact_id));

      const auto& q = sim.state().ship_orders.at(terran_ship).queue;
      N4X_ASSERT(!q.empty());
      N4X_ASSERT(std::holds_alternative<nebula4x::AttackShip>(q.back()));
      const auto& ord = std::get<nebula4x::AttackShip>(q.back());
      N4X_ASSERT(ord.target_ship_id == pirate_contact_id);
      N4X_ASSERT(ord.has_last_known);
      N4X_ASSERT(std::abs(ord.last_known_position_mkm.x - contact.last_seen_position_mkm.x) < 1e-6);
      N4X_ASSERT(std::abs(ord.last_known_position_mkm.y - contact.last_seen_position_mkm.y) < 1e-6);
    }
  }

  // --- exploration discovery + multi-system routing sanity check ---
  {
    nebula4x::ContentDB content;

    // Minimal installations (the scenario references these ids).
    nebula4x::InstallationDef mine;
    mine.id = "automated_mine";
    mine.name = "Automated Mine";
    mine.produces_per_day = {{"Duranium", 0.0}};
    content.installations[mine.id] = mine;

    nebula4x::InstallationDef yard;
    yard.id = "shipyard";
    yard.name = "Shipyard";
    yard.build_rate_tons_per_day = 0.0;
    content.installations[yard.id] = yard;

    auto make_design = [](const std::string& id) {
      nebula4x::ShipDesign d;
      d.id = id;
      d.name = id;
      d.max_hp = 10.0;
      d.speed_km_s = 0.0;
      d.sensor_range_mkm = 0.0;
      return d;
    };

    content.designs["freighter_alpha"] = make_design("freighter_alpha");
    content.designs["surveyor_beta"] = make_design("surveyor_beta");
    content.designs["escort_gamma"] = make_design("escort_gamma");
    content.designs["pirate_raider"] = make_design("pirate_raider");

    nebula4x::Simulation sim(std::move(content), nebula4x::SimConfig{});

    const auto terrans = find_faction_id(sim.state(), "Terran Union");
    const auto pirates = find_faction_id(sim.state(), "Pirate Raiders");
    N4X_ASSERT(terrans != nebula4x::kInvalidId);
    N4X_ASSERT(pirates != nebula4x::kInvalidId);

    const auto sol = find_system_id(sim.state(), "Sol");
    const auto cen = find_system_id(sim.state(), "Alpha Centauri");
    const auto bar = find_system_id(sim.state(), "Barnard's Star");
    N4X_ASSERT(sol != nebula4x::kInvalidId);
    N4X_ASSERT(cen != nebula4x::kInvalidId);
    N4X_ASSERT(bar != nebula4x::kInvalidId);

    // Starting discovery should include the system where each faction has assets.
    N4X_ASSERT(sim.is_system_discovered_by_faction(terrans, sol));
    N4X_ASSERT(!sim.is_system_discovered_by_faction(terrans, cen));
    N4X_ASSERT(!sim.is_system_discovered_by_faction(terrans, bar));

    N4X_ASSERT(sim.is_system_discovered_by_faction(pirates, cen));
    N4X_ASSERT(!sim.is_system_discovered_by_faction(pirates, sol));

    // Find a Terran ship in Sol.
    const auto terran_ship = find_ship_id(sim.state(), "Surveyor Beta");
    N4X_ASSERT(terran_ship != nebula4x::kInvalidId);

    // Find the Sol-side jump point to Alpha Centauri.
    nebula4x::Id sol_jump = nebula4x::kInvalidId;
    for (const auto& [jid, jp] : sim.state().jump_points) {
      if (jp.system_id != sol) continue;
      const auto* dest = nebula4x::find_ptr(sim.state().jump_points, jp.linked_jump_id);
      if (dest && dest->system_id == cen) {
        sol_jump = jid;
        break;
      }
    }
    N4X_ASSERT(sol_jump != nebula4x::kInvalidId);

    // Find the Centauri-side jump point to Barnard.
    nebula4x::Id cen_to_bar = nebula4x::kInvalidId;
    for (const auto& [jid, jp] : sim.state().jump_points) {
      if (jp.system_id != cen) continue;
      const auto* dest = nebula4x::find_ptr(sim.state().jump_points, jp.linked_jump_id);
      if (dest && dest->system_id == bar) {
        cen_to_bar = jid;
        break;
      }
    }
    N4X_ASSERT(cen_to_bar != nebula4x::kInvalidId);

    // Discovery-restricted routing should fail because Barnard isn't discovered yet.
    N4X_ASSERT(!sim.issue_travel_to_system(terran_ship, bar, true));

    // Force a high speed so we can reach the 2nd jump point in one day.
    sim.state().ships[terran_ship].speed_km_s = 100000.0;

    // Put the ship exactly at the Sol jump point, then route all the way to Barnard.
    sim.state().ships[terran_ship].position_mkm = sim.state().jump_points[sol_jump].position_mkm;
    N4X_ASSERT(sim.issue_travel_to_system(terran_ship, bar, false));

    // We expect two TravelViaJump steps: Sol->Centauri and Centauri->Barnard.
    const auto& q = sim.state().ship_orders.at(terran_ship).queue;
    N4X_ASSERT(q.size() >= 2);
    N4X_ASSERT(std::holds_alternative<nebula4x::TravelViaJump>(q[0]));
    N4X_ASSERT(std::holds_alternative<nebula4x::TravelViaJump>(q[1]));
    N4X_ASSERT(std::get<nebula4x::TravelViaJump>(q[0]).jump_point_id == sol_jump);
    N4X_ASSERT(std::get<nebula4x::TravelViaJump>(q[1]).jump_point_id == cen_to_bar);

    sim.advance_days(2);

    N4X_ASSERT(sim.state().ships[terran_ship].system_id == bar);
    N4X_ASSERT(sim.is_system_discovered_by_faction(terrans, cen));
    N4X_ASSERT(sim.is_system_discovered_by_faction(terrans, bar));
  }

  // --- distance-weighted jump routing prefers shorter in-system travel over hop count ---
  {
    nebula4x::ContentDB content;

    // Minimal design so ships have a non-zero speed / hp.
    nebula4x::ShipDesign d;
    d.id = "test_ship";
    d.name = "Test Ship";
    d.speed_km_s = 100.0;
    d.max_hp = 10.0;
    content.designs[d.id] = d;

    nebula4x::Simulation sim(std::move(content), nebula4x::SimConfig{});

    nebula4x::GameState st;
    st.save_version = sim.state().save_version;
    st.date = nebula4x::Date::from_ymd(2200, 1, 1);
    st.next_id = 1;

    // Faction.
    nebula4x::Faction f;
    f.id = nebula4x::allocate_id(st);
    f.name = "Test";
    st.factions[f.id] = f;

    auto add_system = [&](const std::string& name, const nebula4x::Vec2& pos) {
      nebula4x::StarSystem sys;
      sys.id = nebula4x::allocate_id(st);
      sys.name = name;
      sys.galaxy_pos = pos;
      st.systems[sys.id] = sys;
      return sys.id;
    };

    const nebula4x::Id a = add_system("A", nebula4x::Vec2{0.0, 0.0});
    const nebula4x::Id b = add_system("B", nebula4x::Vec2{1.0, 0.0});
    const nebula4x::Id c = add_system("C", nebula4x::Vec2{2.0, 0.0});
    const nebula4x::Id dsys = add_system("D", nebula4x::Vec2{3.0, 0.0});

    auto add_jump_pair = [&](nebula4x::Id sys1, const std::string& name1, const nebula4x::Vec2& pos1,
                             nebula4x::Id sys2, const std::string& name2, const nebula4x::Vec2& pos2) {
      nebula4x::JumpPoint j1;
      j1.id = nebula4x::allocate_id(st);
      j1.name = name1;
      j1.system_id = sys1;
      j1.position_mkm = pos1;

      nebula4x::JumpPoint j2;
      j2.id = nebula4x::allocate_id(st);
      j2.name = name2;
      j2.system_id = sys2;
      j2.position_mkm = pos2;

      j1.linked_jump_id = j2.id;
      j2.linked_jump_id = j1.id;

      st.jump_points[j1.id] = j1;
      st.jump_points[j2.id] = j2;

      st.systems[sys1].jump_points.push_back(j1.id);
      st.systems[sys2].jump_points.push_back(j2.id);
      return std::pair<nebula4x::Id, nebula4x::Id>{j1.id, j2.id};
    };

    // Cheap 3-hop route A->B->C->D (total approach distance = 3 mkm).
    const auto jp_ab = add_jump_pair(a, "JP-AB", nebula4x::Vec2{1.0, 0.0}, b, "JP-BA", nebula4x::Vec2{0.0, 0.0}).first;
    const auto jp_bc = add_jump_pair(b, "JP-BC", nebula4x::Vec2{1.0, 0.0}, c, "JP-CB", nebula4x::Vec2{0.0, 0.0}).first;
    const auto jp_cd = add_jump_pair(c, "JP-CD", nebula4x::Vec2{1.0, 0.0}, dsys, "JP-DC", nebula4x::Vec2{0.0, 0.0}).first;

    // Tempting 1-hop route A->D but far away (approach distance = 1000 mkm).
    const auto jp_ad = add_jump_pair(a, "JP-AD", nebula4x::Vec2{1000.0, 0.0}, dsys, "JP-DA", nebula4x::Vec2{0.0, 0.0}).first;

    // Ship in system A at the origin.
    nebula4x::Ship sh;
    sh.id = nebula4x::allocate_id(st);
    sh.name = "S";
    sh.faction_id = f.id;
    sh.system_id = a;
    sh.position_mkm = nebula4x::Vec2{0.0, 0.0};
    sh.design_id = "test_ship";
    st.ships[sh.id] = sh;
    st.systems[a].ships.push_back(sh.id);
    st.selected_system = a;

    const nebula4x::Id ship_id = sh.id;

    sim.load_game(st);

    N4X_ASSERT(sim.clear_orders(ship_id));
    N4X_ASSERT(sim.issue_travel_to_system(ship_id, dsys, false));

    const auto& q = sim.state().ship_orders.at(ship_id).queue;
    N4X_ASSERT(q.size() == 3);
    N4X_ASSERT(std::get<nebula4x::TravelViaJump>(q[0]).jump_point_id == jp_ab);
    N4X_ASSERT(std::get<nebula4x::TravelViaJump>(q[1]).jump_point_id == jp_bc);
    N4X_ASSERT(std::get<nebula4x::TravelViaJump>(q[2]).jump_point_id == jp_cd);

    auto plan = sim.plan_jump_route_for_ship(ship_id, dsys, false, false);
    N4X_ASSERT(plan.has_value());
    N4X_ASSERT(plan->jump_ids.size() == 3);
    N4X_ASSERT(plan->jump_ids[0] == jp_ab);
    N4X_ASSERT(plan->jump_ids[1] == jp_bc);
    N4X_ASSERT(plan->jump_ids[2] == jp_cd);
    N4X_ASSERT(plan->systems.size() == 4);
    N4X_ASSERT(plan->systems.front() == a);
    N4X_ASSERT(plan->systems.back() == dsys);
    N4X_ASSERT(plan->distance_mkm < 10.0);

    // Make sure we did not pick the direct jump.
    N4X_ASSERT(plan->jump_ids[0] != jp_ad);
  }



  // --- shipyard guard + no accidental installation insertion ---
  // Colonies without a shipyard shouldn't grow a zero-count "shipyard" entry just by ticking,
  // and enqueuing a ship build at such a colony should fail.
  {
    nebula4x::ContentDB content;

    // Minimal installations to satisfy the scenario content references + shipyard ticking.
    nebula4x::InstallationDef mine;
    mine.id = "automated_mine";
    mine.name = "Automated Mine";
    mine.produces_per_day = {{"Duranium", 0.0}};
    content.installations[mine.id] = mine;

    nebula4x::InstallationDef yard;
    yard.id = "shipyard";
    yard.name = "Shipyard";
    yard.build_rate_tons_per_day = 50;
    content.installations[yard.id] = yard;

    // Minimal design to allow calling enqueue_build.
    nebula4x::ShipDesign d;
    d.id = "freighter_alpha";
    d.name = "Freighter Alpha";
    d.mass_tons = 100;
    d.speed_km_s = 0.0;
    content.designs[d.id] = d;

    nebula4x::Simulation sim(std::move(content), nebula4x::SimConfig{});

    const auto mars_id = find_colony_id(sim.state(), "Mars Outpost");
    N4X_ASSERT(mars_id != nebula4x::kInvalidId);

    // Mars starts without a shipyard.
    N4X_ASSERT(sim.state().colonies[mars_id].installations.count("shipyard") == 0);

    // Ticking shouldn't insert a 0-count shipyard entry.
    sim.advance_days(1);
    N4X_ASSERT(sim.state().colonies[mars_id].installations.count("shipyard") == 0);

    // And enqueuing a ship build should fail without a shipyard present.
    N4X_ASSERT(!sim.enqueue_build(mars_id, "freighter_alpha"));
  }

  // --- research queue prereq enforcement + planning sanity check ---
  // A faction should be able to queue techs out of prereq order; the sim will
  // automatically pick the first available tech whose prereqs are met.
  {
    nebula4x::ContentDB content;

    // Minimal installations: avoid generating extra RP from the default scenario colonies.
    nebula4x::InstallationDef lab;
    lab.id = "research_lab";
    lab.name = "Research Lab";
    lab.research_points_per_day = 0.0;
    content.installations[lab.id] = lab;

    // Minimal designs for scenario ships.
    auto make_min_design = [](const std::string& id) {
      nebula4x::ShipDesign d;
      d.id = id;
      d.name = id;
      d.max_hp = 10.0;
      d.speed_km_s = 0.0;
      d.sensor_range_mkm = 0.0;
      return d;
    };
    content.designs["freighter_alpha"] = make_min_design("freighter_alpha");
    content.designs["surveyor_beta"] = make_min_design("surveyor_beta");
    content.designs["escort_gamma"] = make_min_design("escort_gamma");
    content.designs["pirate_raider"] = make_min_design("pirate_raider");

    // Tech A (no prereqs), Tech B (requires A).
    nebula4x::TechDef tech_a;
    tech_a.id = "tech_a";
    tech_a.name = "Tech A";
    tech_a.cost = 10.0;

    nebula4x::TechDef tech_b;
    tech_b.id = "tech_b";
    tech_b.name = "Tech B";
    tech_b.cost = 10.0;
    tech_b.prereqs = {"tech_a"};

    content.techs[tech_a.id] = tech_a;
    content.techs[tech_b.id] = tech_b;

    nebula4x::Simulation sim(std::move(content), nebula4x::SimConfig{});

    const auto terrans_id = find_faction_id(sim.state(), "Terran Union");
    N4X_ASSERT(terrans_id != nebula4x::kInvalidId);

    auto& fac = sim.state().factions[terrans_id];
    fac.known_techs.clear();
    fac.unlocked_components.clear();
    fac.unlocked_installations.clear();
    fac.active_research_id.clear();
    fac.active_research_progress = 0.0;

    // Intentionally queue out of prereq order: B depends on A.
    fac.research_queue = {"tech_b", "tech_a"};

    // Enough RP to complete both in one tick.
    fac.research_points = 25.0;

    sim.advance_days(1);

    auto has = [&](const std::string& id) {
      for (const auto& x : fac.known_techs) {
        if (x == id) return true;
      }
      return false;
    };

    N4X_ASSERT(has("tech_a"));
    N4X_ASSERT(has("tech_b"));
    N4X_ASSERT(fac.active_research_id.empty());
    N4X_ASSERT(fac.research_queue.empty());
  }

  // Active research blocked by prereqs should not deadlock the system.
  {
    nebula4x::ContentDB content;

    nebula4x::InstallationDef lab;
    lab.id = "research_lab";
    lab.name = "Research Lab";
    lab.research_points_per_day = 0.0;
    content.installations[lab.id] = lab;

    auto make_min_design = [](const std::string& id) {
      nebula4x::ShipDesign d;
      d.id = id;
      d.name = id;
      d.max_hp = 10.0;
      d.speed_km_s = 0.0;
      d.sensor_range_mkm = 0.0;
      return d;
    };
    content.designs["freighter_alpha"] = make_min_design("freighter_alpha");
    content.designs["surveyor_beta"] = make_min_design("surveyor_beta");
    content.designs["escort_gamma"] = make_min_design("escort_gamma");
    content.designs["pirate_raider"] = make_min_design("pirate_raider");

    nebula4x::TechDef tech_a;
    tech_a.id = "tech_a";
    tech_a.name = "Tech A";
    tech_a.cost = 10.0;

    nebula4x::TechDef tech_b;
    tech_b.id = "tech_b";
    tech_b.name = "Tech B";
    tech_b.cost = 10.0;
    tech_b.prereqs = {"tech_a"};

    content.techs[tech_a.id] = tech_a;
    content.techs[tech_b.id] = tech_b;

    nebula4x::Simulation sim(std::move(content), nebula4x::SimConfig{});

    const auto terrans_id = find_faction_id(sim.state(), "Terran Union");
    N4X_ASSERT(terrans_id != nebula4x::kInvalidId);

    auto& fac = sim.state().factions[terrans_id];
    fac.known_techs.clear();
    fac.unlocked_components.clear();
    fac.unlocked_installations.clear();

    // Force an invalid active selection (blocked by prereqs). The sim should
    // requeue it and progress A instead.
    fac.active_research_id = "tech_b";
    fac.active_research_progress = 0.0;
    fac.research_queue = {"tech_a"};
    fac.research_points = 15.0;

    sim.advance_days(1);

    auto has = [&](const std::string& id) {
      for (const auto& x : fac.known_techs) {
        if (x == id) return true;
      }
      return false;
    };

    // Should have completed A, then started B with remaining RP.
    N4X_ASSERT(has("tech_a"));
    N4X_ASSERT(!has("tech_b"));
    N4X_ASSERT(fac.active_research_id == "tech_b");
    N4X_ASSERT(std::fabs(fac.active_research_progress - 5.0) < 1e-6);
    N4X_ASSERT(fac.research_queue.empty());
  }

  // Event log should record exploration + contact state changes.
  {
    nebula4x::ContentDB content;

    auto make_min_design = [](const std::string& id, double speed_km_s, double sensor_range_mkm) {
      nebula4x::ShipDesign d;
      d.id = id;
      d.name = id;
      d.max_hp = 10.0;
      d.speed_km_s = speed_km_s;
      d.sensor_range_mkm = sensor_range_mkm;
      return d;
    };

    content.designs["freighter_alpha"] = make_min_design("freighter_alpha", 0.0, 0.0);
    content.designs["surveyor_beta"] = make_min_design("surveyor_beta", 1000000.0, 1000.0);
    content.designs["escort_gamma"] = make_min_design("escort_gamma", 0.0, 0.0);
    content.designs["pirate_raider"] = make_min_design("pirate_raider", 0.0, 0.0);

    nebula4x::Simulation sim(std::move(content), nebula4x::SimConfig{});

    const auto terrans_id = find_faction_id(sim.state(), "Terran Union");
    N4X_ASSERT(terrans_id != nebula4x::kInvalidId);

    const auto surveyor_id = find_ship_id(sim.state(), "Surveyor Beta");
    N4X_ASSERT(surveyor_id != nebula4x::kInvalidId);

    auto find_jump_id = [](const nebula4x::GameState& s, const std::string& name) {
      for (const auto& [id, jp] : s.jump_points) {
        if (jp.name == name) return id;
      }
      return nebula4x::kInvalidId;
    };

    const auto sol_jump = find_jump_id(sim.state(), "Sol Jump Point");
    N4X_ASSERT(sol_jump != nebula4x::kInvalidId);

    // Transit to Alpha Centauri in one day (very fast ship).
    N4X_ASSERT(sim.issue_travel_via_jump(surveyor_id, sol_jump));
    sim.advance_days(1);

    bool saw_discovery = false;
    bool saw_contact = false;
    for (const auto& ev : sim.state().events) {
      if (ev.category == nebula4x::EventCategory::Exploration &&
          ev.message.find("discovered system Alpha Centauri") != std::string::npos) {
        saw_discovery = true;
      }
      if (ev.category == nebula4x::EventCategory::Intel &&
          ev.message.find("New contact for Terran Union") != std::string::npos) {
        saw_contact = true;
      }
    }

    N4X_ASSERT(saw_discovery);
    N4X_ASSERT(saw_contact);
  }

  // Event seq ids should be monotonic and advance_until_event should pause on a matching new event.
  {
    auto make_content = []() {
      nebula4x::ContentDB content;

      nebula4x::InstallationDef mine;
      mine.id = "automated_mine";
      mine.name = "Automated Mine";
      mine.construction_cost = 200.0; // should take multiple days with baseline CP
      content.installations[mine.id] = mine;

      nebula4x::InstallationDef yard;
      yard.id = "shipyard";
      yard.name = "Shipyard";
      yard.build_rate_tons_per_day = 0.0;
      content.installations[yard.id] = yard;

      // Minimal designs referenced by scenario.
      auto make_min_design = [](const std::string& id) {
        nebula4x::ShipDesign d;
        d.id = id;
        d.name = id;
        d.max_hp = 10.0;
        d.speed_km_s = 0.0;
        d.sensor_range_mkm = 0.0;
        return d;
      };
      content.designs["freighter_alpha"] = make_min_design("freighter_alpha");
      content.designs["surveyor_beta"] = make_min_design("surveyor_beta");
      content.designs["escort_gamma"] = make_min_design("escort_gamma");
      content.designs["pirate_raider"] = make_min_design("pirate_raider");

      return content;
    };

    nebula4x::Simulation sim(make_content(), nebula4x::SimConfig{});

    const auto earth_id = find_colony_id(sim.state(), "Earth");
    N4X_ASSERT(earth_id != nebula4x::kInvalidId);

    N4X_ASSERT(sim.enqueue_installation_build(earth_id, "automated_mine", 1));

    nebula4x::EventStopCondition stop;
    stop.stop_on_info = true;
    stop.stop_on_warn = true;
    stop.stop_on_error = true;
    stop.filter_category = true;
    stop.category = nebula4x::EventCategory::Construction;
    stop.message_contains = "Automated Mine";

    auto r = sim.advance_until_event(50, stop);
    N4X_ASSERT(r.hit);
    N4X_ASSERT(r.days_advanced > 0 && r.days_advanced <= 50);
    N4X_ASSERT(r.event.category == nebula4x::EventCategory::Construction);
    N4X_ASSERT(r.event.message.find("Automated Mine") != std::string::npos);
    N4X_ASSERT(r.event.seq > 0);

    // Seq should be strictly increasing in the in-memory log.
    std::uint64_t prev = 0;
    for (const auto& ev : sim.state().events) {
      N4X_ASSERT(ev.seq > prev);
      prev = ev.seq;
    }

    // Round-trip serialization should preserve seq and keep next_event_seq ahead.
    const std::string json_text = nebula4x::serialize_game_to_json(sim.state());
    const auto loaded = nebula4x::deserialize_game_from_json(json_text);

    std::uint64_t prev2 = 0;
    for (const auto& ev : loaded.events) {
      N4X_ASSERT(ev.seq > prev2);
      prev2 = ev.seq;
    }
    N4X_ASSERT(loaded.next_event_seq > prev2);

    // Negative filter: if the substring doesn't match, advance_until_event should
    // not stop even if construction events occur.
    {
      nebula4x::Simulation sim2(make_content(), nebula4x::SimConfig{});
      const auto earth2_id = find_colony_id(sim2.state(), "Earth");
      N4X_ASSERT(earth2_id != nebula4x::kInvalidId);
      N4X_ASSERT(sim2.enqueue_installation_build(earth2_id, "automated_mine", 1));

      nebula4x::EventStopCondition stop2;
      stop2.stop_on_info = true;
      stop2.stop_on_warn = true;
      stop2.stop_on_error = true;
      stop2.filter_category = true;
      stop2.category = nebula4x::EventCategory::Construction;
      stop2.message_contains = "THIS SUBSTRING DOES NOT EXIST";

      auto r2 = sim2.advance_until_event(50, stop2);
      N4X_ASSERT(!r2.hit);
      N4X_ASSERT(r2.days_advanced == 50);
    }
  }

  return 0;
}
