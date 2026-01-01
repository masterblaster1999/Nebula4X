#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "nebula4x/core/simulation.h"
#include "nebula4x/core/serialization.h"
#include "nebula4x/core/tech.h"

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

  // --- ship-to-ship fuel transfer sanity check ---
  // Transfer fuel directly between two friendly ships in space.
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

    auto make_min_design = [](const std::string& id) {
      nebula4x::ShipDesign d;
      d.id = id;
      d.name = id;
      d.max_hp = 10.0;
      d.speed_km_s = 0.0;
      d.sensor_range_mkm = 0.0;
      return d;
    };

    nebula4x::ShipDesign tanker = make_min_design("freighter_alpha");
    tanker.name = "Freighter Alpha";
    tanker.fuel_capacity_tons = 100.0;
    content.designs[tanker.id] = tanker;

    nebula4x::ShipDesign receiver = make_min_design("escort_gamma");
    receiver.name = "Escort Gamma";
    receiver.fuel_capacity_tons = 50.0;
    content.designs[receiver.id] = receiver;

    // Other scenario ships.
    content.designs["surveyor_beta"] = make_min_design("surveyor_beta");
    content.designs["pirate_raider"] = make_min_design("pirate_raider");

    nebula4x::Simulation sim(std::move(content), nebula4x::SimConfig{});

    const auto tanker_id = find_ship_id(sim.state(), "Freighter Alpha");
    const auto receiver_id = find_ship_id(sim.state(), "Escort Gamma");
    N4X_ASSERT(tanker_id != nebula4x::kInvalidId);
    N4X_ASSERT(receiver_id != nebula4x::kInvalidId);

    // Place both ships together away from colonies so the auto-refuel tick doesn't
    // overwrite the manual fuel setup.
    auto* sh_tanker = nebula4x::find_ptr(sim.state().ships, tanker_id);
    auto* sh_recv = nebula4x::find_ptr(sim.state().ships, receiver_id);
    N4X_ASSERT(sh_tanker);
    N4X_ASSERT(sh_recv);

    sh_recv->system_id = sh_tanker->system_id;
    sh_tanker->position_mkm = {0.0, 0.0};
    sh_recv->position_mkm = {0.0, 0.0};

    sh_tanker->fuel_tons = 80.0;
    sh_recv->fuel_tons = 0.0;

    N4X_ASSERT(sim.issue_transfer_fuel_to_ship(tanker_id, receiver_id, 30.0));

    sim.advance_days(1);

    const auto* tanker_after = nebula4x::find_ptr(sim.state().ships, tanker_id);
    const auto* recv_after = nebula4x::find_ptr(sim.state().ships, receiver_id);
    N4X_ASSERT(tanker_after);
    N4X_ASSERT(recv_after);

    N4X_ASSERT(std::abs(tanker_after->fuel_tons - 50.0) < 1e-6);
    N4X_ASSERT(std::abs(recv_after->fuel_tons - 30.0) < 1e-6);
  }

  // --- ship-to-ship troop transfer sanity check ---
  // Transfer embarked troops directly between two friendly ships.
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

    auto make_min_design = [](const std::string& id) {
      nebula4x::ShipDesign d;
      d.id = id;
      d.name = id;
      d.max_hp = 10.0;
      d.speed_km_s = 0.0;
      d.sensor_range_mkm = 0.0;
      return d;
    };

    nebula4x::ShipDesign transport = make_min_design("freighter_alpha");
    transport.name = "Freighter Alpha";
    transport.troop_capacity = 100.0;
    content.designs[transport.id] = transport;

    nebula4x::ShipDesign carrier = make_min_design("escort_gamma");
    carrier.name = "Escort Gamma";
    carrier.troop_capacity = 50.0;
    content.designs[carrier.id] = carrier;

    // Other scenario ships.
    content.designs["surveyor_beta"] = make_min_design("surveyor_beta");
    content.designs["pirate_raider"] = make_min_design("pirate_raider");

    nebula4x::Simulation sim(std::move(content), nebula4x::SimConfig{});

    const auto transport_id = find_ship_id(sim.state(), "Freighter Alpha");
    const auto carrier_id = find_ship_id(sim.state(), "Escort Gamma");
    N4X_ASSERT(transport_id != nebula4x::kInvalidId);
    N4X_ASSERT(carrier_id != nebula4x::kInvalidId);

    auto* sh_src = nebula4x::find_ptr(sim.state().ships, transport_id);
    auto* sh_tgt = nebula4x::find_ptr(sim.state().ships, carrier_id);
    N4X_ASSERT(sh_src);
    N4X_ASSERT(sh_tgt);

    sh_tgt->system_id = sh_src->system_id;
    sh_src->position_mkm = {0.0, 0.0};
    sh_tgt->position_mkm = {0.0, 0.0};

    sh_src->troops = 80.0;
    sh_tgt->troops = 10.0;

    // strength <= 0 => transfer as much as possible (bounded by target free capacity).
    N4X_ASSERT(sim.issue_transfer_troops_to_ship(transport_id, carrier_id, 0.0));

    sim.advance_days(1);

    const auto* src_after = nebula4x::find_ptr(sim.state().ships, transport_id);
    const auto* tgt_after = nebula4x::find_ptr(sim.state().ships, carrier_id);
    N4X_ASSERT(src_after);
    N4X_ASSERT(tgt_after);

    // Target starts with 10 and has 50 capacity -> can receive 40.
    N4X_ASSERT(std::abs(src_after->troops - 40.0) < 1e-6);
    N4X_ASSERT(std::abs(tgt_after->troops - 50.0) < 1e-6);
  }

  // --- auto-refuel (idle) sanity check ---
  // When enabled and fuel is below threshold, an idle ship should queue a refuel trip.
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

    auto make_min_design = [](const std::string& id) {
      nebula4x::ShipDesign d;
      d.id = id;
      d.name = id;
      d.max_hp = 10.0;
      d.speed_km_s = 10.0;
      d.sensor_range_mkm = 0.0;
      return d;
    };

    // Give the surveyor a fuel tank + usage so it can run low.
    nebula4x::ShipDesign surveyor = make_min_design("surveyor_beta");
    surveyor.name = "Surveyor Beta";
    surveyor.fuel_capacity_tons = 100.0;
    surveyor.fuel_use_per_mkm = 1.0;
    content.designs[surveyor.id] = surveyor;

    // Other scenario ships.
    content.designs["freighter_alpha"] = make_min_design("freighter_alpha");
    content.designs["escort_gamma"] = make_min_design("escort_gamma");
    content.designs["pirate_raider"] = make_min_design("pirate_raider");

    nebula4x::Simulation sim(std::move(content), nebula4x::SimConfig{});

    const auto earth_id = find_colony_id(sim.state(), "Earth");
    N4X_ASSERT(earth_id != nebula4x::kInvalidId);
    const auto ship_id = find_ship_id(sim.state(), "Surveyor Beta");
    N4X_ASSERT(ship_id != nebula4x::kInvalidId);

    // Ensure exactly one refuel source so the destination is deterministic.
    for (auto& [cid, col] : sim.state().colonies) {
      col.minerals["Fuel"] = 0.0;
    }
    sim.state().colonies[earth_id].minerals["Fuel"] = 1000.0;

    auto* sh = nebula4x::find_ptr(sim.state().ships, ship_id);
    N4X_ASSERT(sh);
    // Ensure we are not already docked at Earth.
    sh->position_mkm = {0.0, 0.0};
    sh->fuel_tons = 5.0; // 5% full
    sh->auto_refuel = true;
    sh->auto_refuel_threshold_fraction = 0.50;

    N4X_ASSERT(sim.clear_orders(ship_id));

    sim.advance_days(1);

    const auto* so = nebula4x::find_ptr(sim.state().ship_orders, ship_id);
    N4X_ASSERT(so);
    N4X_ASSERT(!so->queue.empty());

    // Auto-refuel should enqueue a MoveToBody toward Earth's body (possibly preceded by travel orders).
    const nebula4x::Id earth_body_id = sim.state().colonies.at(earth_id).body_id;
    const auto& last = so->queue.back();
    N4X_ASSERT(std::holds_alternative<nebula4x::MoveToBody>(last));
    N4X_ASSERT(std::get<nebula4x::MoveToBody>(last).body_id == earth_body_id);
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


  // --- goal-aware jump routing prefers the best destination entry jump ---
  // When multiple entry jump points exist in the destination system, we should prefer the
  // route that minimizes *total* travel (jump-network + final in-system leg to the goal).
  {
    nebula4x::ContentDB content;

    nebula4x::ShipDesign d;
    d.id = "probe";
    d.name = "Probe";
    d.speed_km_s = 1000.0;
    d.mass_tons = 100.0;
    content.designs[d.id] = d;

    nebula4x::Simulation sim(std::move(content), nebula4x::SimConfig{});

    nebula4x::GameState st;

    const nebula4x::Id fid = st.next_id++;
    nebula4x::Faction f;
    f.id = fid;
    f.name = "TestFaction";
    st.factions[fid] = f;

    const nebula4x::Id a = st.next_id++;
    const nebula4x::Id b = st.next_id++;
    const nebula4x::Id dsys = st.next_id++;
    st.systems[a] = nebula4x::StarSystem{.id = a, .name = "A"};
    st.systems[b] = nebula4x::StarSystem{.id = b, .name = "B"};
    st.systems[dsys] = nebula4x::StarSystem{.id = dsys, .name = "D"};

    auto add_pair = [&](nebula4x::Id sys1, nebula4x::Id sys2, nebula4x::Vec2 p1, nebula4x::Vec2 p2) -> std::pair<nebula4x::Id, nebula4x::Id> {
      const nebula4x::Id j1 = st.next_id++;
      const nebula4x::Id j2 = st.next_id++;
      st.jump_points[j1] = nebula4x::JumpPoint{.id = j1, .system_id = sys1, .position_mkm = p1, .linked_jump_id = j2};
      st.jump_points[j2] = nebula4x::JumpPoint{.id = j2, .system_id = sys2, .position_mkm = p2, .linked_jump_id = j1};
      st.systems[sys1].jump_points.push_back(j1);
      st.systems[sys2].jump_points.push_back(j2);
      return {j1, j2};
    };

    // Direct A<->D: cheap to reach in A, but arrives far from the goal in D.
    const auto [jp_ad, jp_da] = add_pair(a, dsys, nebula4x::Vec2{1.0, 0.0}, nebula4x::Vec2{1000.0, 0.0});
    // A<->B and B<->D: slightly more expensive upfront, but arrives near the goal.
    const auto [jp_ab, jp_ba] = add_pair(a, b, nebula4x::Vec2{2.0, 0.0}, nebula4x::Vec2{0.0, 0.0});
    const auto [jp_bd, jp_db] = add_pair(b, dsys, nebula4x::Vec2{0.0, 0.0}, nebula4x::Vec2{0.0, 0.0});

    // Target body in system D at the goal position.
    const nebula4x::Id body_id = st.next_id++;
    nebula4x::Body body;
    body.id = body_id;
    body.name = "GoalBody";
    body.system_id = dsys;
    body.position_mkm = nebula4x::Vec2{0.0, 0.0};
    st.bodies[body_id] = body;
    st.systems[dsys].bodies.push_back(body_id);

    // Ship starts in A at origin.
    const nebula4x::Id ship_id = st.next_id++;
    nebula4x::Ship ship;
    ship.id = ship_id;
    ship.name = "TestShip";
    ship.faction_id = fid;
    ship.system_id = a;
    ship.position_mkm = nebula4x::Vec2{0.0, 0.0};
    ship.speed_km_s = 1000.0;
    ship.design_id = "probe";
    st.ships[ship_id] = ship;

    sim.load_game(st);

    // System-only route prefers the direct jump (cheapest to enter system D).
    {
      auto plan = sim.plan_jump_route_for_ship(ship_id, dsys, false, false);
      N4X_ASSERT(plan.has_value());
      N4X_ASSERT(plan->jump_ids.size() == 1);
      N4X_ASSERT(plan->jump_ids[0] == jp_ad);
    }

    // Goal-aware route prefers the entry jump closest to the goal position in D.
    {
      auto plan = sim.plan_jump_route_for_ship_to_pos(ship_id, dsys, nebula4x::Vec2{0.0, 0.0}, false, false);
      N4X_ASSERT(plan.has_value());
      N4X_ASSERT(plan->jump_ids.size() == 2);
      N4X_ASSERT(plan->jump_ids[0] == jp_ab);
      N4X_ASSERT(plan->jump_ids[1] == jp_bd);
      N4X_ASSERT(plan->has_goal_pos);
      N4X_ASSERT(plan->final_leg_mkm == 0.0);
    }

    // And order issuing (MoveToBody) should use the goal-aware routing as well.
    N4X_ASSERT(sim.clear_orders(ship_id));
    N4X_ASSERT(sim.issue_move_to_body(ship_id, body_id, false));

    const auto& q = sim.state().ship_orders.at(ship_id).queue;
    N4X_ASSERT(q.size() == 3);
    N4X_ASSERT(std::get<nebula4x::TravelViaJump>(q[0]).jump_point_id == jp_ab);
    N4X_ASSERT(std::get<nebula4x::TravelViaJump>(q[1]).jump_point_id == jp_bd);
    N4X_ASSERT(std::get<nebula4x::MoveToBody>(q[2]).body_id == body_id);

    // Make sure we did not pick the direct jump.
    N4X_ASSERT(std::get<nebula4x::TravelViaJump>(q[0]).jump_point_id != jp_ad);
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

  // --- planetary defense weapon sanity check ---
  // A colony with a weapon installation should fire at detected hostile ships
  // that enter range.
  {
    nebula4x::ContentDB content;

    // Installations referenced by the default scenario.
    nebula4x::InstallationDef sensor;
    sensor.id = "sensor_station";
    sensor.name = "Sensor Station";
    sensor.sensor_range_mkm = 2000.0; // ensure detection within the Sol system map scale
    content.installations[sensor.id] = sensor;

    nebula4x::InstallationDef fortress;
    fortress.id = "planetary_fortress";
    fortress.name = "Planetary Fortress";
    fortress.weapon_damage = 12.0;
    fortress.weapon_range_mkm = 25.0;
    content.installations[fortress.id] = fortress;

    // Minimal design so the pirate ship has meaningful HP.
    nebula4x::ShipDesign raider;
    raider.id = "pirate_raider";
    raider.name = "Pirate Raider";
    raider.max_hp = 100.0;
    raider.speed_km_s = 0.0;
    content.designs[raider.id] = raider;

    nebula4x::Simulation sim(std::move(content), nebula4x::SimConfig{});

    const auto sol_id = find_system_id(sim.state(), "Sol");
    N4X_ASSERT(sol_id != nebula4x::kInvalidId);

    const auto earth_id = find_colony_id(sim.state(), "Earth");
    N4X_ASSERT(earth_id != nebula4x::kInvalidId);

    const auto raider_id = find_ship_id(sim.state(), "Raider I");
    N4X_ASSERT(raider_id != nebula4x::kInvalidId);

    auto& st = sim.state();
    auto& ship = st.ships.at(raider_id);

    // Move the pirate ship into Sol, near Earth, so it should be both detected
    // and within fortress firing range.
    const nebula4x::Id old_sys = ship.system_id;
    if (old_sys != nebula4x::kInvalidId && st.systems.count(old_sys)) {
      auto& vec = st.systems.at(old_sys).ships;
      vec.erase(std::remove(vec.begin(), vec.end(), raider_id), vec.end());
    }

    ship.system_id = sol_id;
    st.systems.at(sol_id).ships.push_back(raider_id);

    const auto& earth_col = st.colonies.at(earth_id);
    const auto& earth_body = st.bodies.at(earth_col.body_id);
    ship.position_mkm = earth_body.position_mkm + nebula4x::Vec2{1.0, 0.0};

    const double hp_before = ship.hp;
    sim.advance_days(1);
    const double hp_after = st.ships.at(raider_id).hp;

    N4X_ASSERT(hp_after < hp_before);
  }


  // Mutual Friendly cooperation: intel + logistics sharing.
  {
    using namespace nebula4x;
    ContentDB content;

    // Minimal ship designs for this test.
    {
      ShipDesign d;
      d.id = "A Dock";
      d.name = "A Dock";
      d.max_hp = 10.0;
      d.fuel_capacity_tons = 100.0;
      d.cargo_tons = 50.0;
      content.designs[d.id] = d;
    }
    {
      ShipDesign d;
      d.id = "B Sensor";
      d.name = "B Sensor";
      d.max_hp = 10.0;
      d.sensor_range_mkm = 100.0;
      content.designs[d.id] = d;
    }
    {
      ShipDesign d;
      d.id = "B Jumper";
      d.name = "B Jumper";
      d.max_hp = 10.0;
      content.designs[d.id] = d;
    }
    {
      ShipDesign d;
      d.id = "C Target";
      d.name = "C Target";
      d.max_hp = 10.0;
      content.designs[d.id] = d;
    }

    Simulation sim(std::move(content), SimConfig{});

    constexpr Id kFacA = 1;
    constexpr Id kFacB = 2;
    constexpr Id kFacC = 3;
    constexpr Id kSys0 = 100;
    constexpr Id kSys1 = 101;
    constexpr Id kBody0 = 200;
    constexpr Id kCol0 = 300;
    constexpr Id kShipBSensor = 400;
    constexpr Id kShipCTarget = 401;
    constexpr Id kShipADock = 402;
    constexpr Id kShipBJumper = 403;
    constexpr Id kJP0 = 500;
    constexpr Id kJP1 = 501;

    GameState s;
    s.date = Date(0);
    s.selected_system = kSys0;
    s.next_id = 10000;

    // Factions
    {
      Faction fa;
      fa.id = kFacA;
      fa.name = "Faction A";
      fa.discovered_systems = {kSys0};
      s.factions[kFacA] = fa;
    }
    {
      Faction fb;
      fb.id = kFacB;
      fb.name = "Faction B";
      fb.discovered_systems = {kSys0};
      s.factions[kFacB] = fb;
    }
    {
      Faction fc;
      fc.id = kFacC;
      fc.name = "Faction C";
      fc.discovered_systems = {kSys0};
      s.factions[kFacC] = fc;
    }

    // Systems
    {
      StarSystem sys0;
      sys0.id = kSys0;
      sys0.name = "Sys0";
      sys0.bodies = {kBody0};
      sys0.ships = {kShipBSensor, kShipCTarget, kShipADock, kShipBJumper};
      sys0.jump_points = {kJP0};
      s.systems[kSys0] = sys0;
    }
    {
      StarSystem sys1;
      sys1.id = kSys1;
      sys1.name = "Sys1";
      sys1.jump_points = {kJP1};
      s.systems[kSys1] = sys1;
    }

    // Body + colony (owned by B at origin)
    {
      Body b;
      b.id = kBody0;
      b.name = "B-Prime";
      b.system_id = kSys0;
      b.type = BodyType::Planet;
      b.orbit_radius_mkm = 0.0;
      b.orbit_period_days = 1.0;
      b.radius_km = 1000.0;
      s.bodies[b.id] = b;
    }
    {
      Colony c;
      c.id = kCol0;
      c.name = "B Colony";
      c.faction_id = kFacB;
      c.body_id = kBody0;
      c.installations["shipyard"] = 2;
      c.minerals["Fuel"] = 500.0;
      c.minerals["Iron"] = 100.0;
      s.colonies[c.id] = c;
    }

    // Jump points Sys0 <-> Sys1
    {
      JumpPoint jp;
      jp.id = kJP0;
      jp.name = "JP0";
      jp.system_id = kSys0;
      jp.position_mkm = {50.0, 0.0};
      jp.linked_jump_id = kJP1;
      s.jump_points[jp.id] = jp;
    }
    {
      JumpPoint jp;
      jp.id = kJP1;
      jp.name = "JP1";
      jp.system_id = kSys1;
      jp.position_mkm = {0.0, 0.0};
      jp.linked_jump_id = kJP0;
      s.jump_points[jp.id] = jp;
    }

    // Ships
    {
      Ship sh;
      sh.id = kShipBSensor;
      sh.name = "B Sensor";
      sh.faction_id = kFacB;
      sh.system_id = kSys0;
      sh.design_id = "B Sensor";
      sh.position_mkm = {0.0, 0.0};
      sh.hp = 10.0;
      s.ships[sh.id] = sh;
    }
    {
      Ship sh;
      sh.id = kShipCTarget;
      sh.name = "C Target";
      sh.faction_id = kFacC;
      sh.system_id = kSys0;
      sh.design_id = "C Target";
      sh.position_mkm = {10.0, 0.0};
      sh.hp = 10.0;
      s.ships[sh.id] = sh;
    }
    {
      Ship sh;
      sh.id = kShipADock;
      sh.name = "A Dock";
      sh.faction_id = kFacA;
      sh.system_id = kSys0;
      sh.design_id = "A Dock";
      sh.position_mkm = {0.0, 0.0};
      sh.hp = 5.0;
      sh.fuel_tons = 0.0;
      s.ships[sh.id] = sh;
    }
    {
      Ship sh;
      sh.id = kShipBJumper;
      sh.name = "B Jumper";
      sh.faction_id = kFacB;
      sh.system_id = kSys0;
      sh.design_id = "B Jumper";
      sh.position_mkm = {50.0, 0.0};
      sh.hp = 10.0;
      s.ships[sh.id] = sh;
    }

    // Load the custom scenario (this also computes initial contacts, etc).
    sim.load_game(std::move(s));

    // A has no sensors and is not friendly with B yet, so should not have contacts for C.
    N4X_ASSERT(sim.state().factions[kFacA].ship_contacts.count(kShipCTarget) == 0);

    // Establish mutual friendship; this should immediately sync B's contacts/map intel to A.
    sim.set_diplomatic_status(kFacA, kFacB, DiplomacyStatus::Friendly, /*reciprocal=*/true, /*push_event_on_change=*/false);
    N4X_ASSERT(sim.state().factions[kFacA].ship_contacts.count(kShipCTarget) == 1);

    // Allied logistics: A should be able to queue a mineral transfer at B's colony.
    N4X_ASSERT(sim.issue_load_mineral(kShipADock, kCol0, "Iron", 10.0, /*use_fog_of_war=*/false));

    // And B should be able to jump-discover a new system, sharing it with A.
    N4X_ASSERT(sim.issue_travel_via_jump(kShipBJumper, kJP0));

    // Advance one day: shared sensors should update contacts, allied colony should refuel/repair, cargo should transfer,
    // and discovery should propagate.
    sim.advance_days(1);

    {
      const auto& facA = sim.state().factions.at(kFacA);
      const auto it = facA.ship_contacts.find(kShipCTarget);
      N4X_ASSERT(it != facA.ship_contacts.end());
      N4X_ASSERT(it->second.last_seen_day == sim.state().date.days_since_epoch());
    }
    {
      const auto& shipA = sim.state().ships.at(kShipADock);
      N4X_ASSERT(shipA.fuel_tons > 0.0);
      N4X_ASSERT(shipA.hp > 5.0);
      const auto it = shipA.cargo.find("Iron");
      N4X_ASSERT(it != shipA.cargo.end());
      N4X_ASSERT(it->second > 0.0);
    }
    {
      const auto& col = sim.state().colonies.at(kCol0);
      N4X_ASSERT(col.minerals.at("Fuel") < 500.0);
      N4X_ASSERT(col.minerals.at("Iron") < 100.0);
    }
    {
      const auto& facA = sim.state().factions.at(kFacA);
      const bool has_sys1 = std::find(facA.discovered_systems.begin(), facA.discovered_systems.end(), kSys1) != facA.discovered_systems.end();
      N4X_ASSERT(has_sys1);
    }
  }

  // --- orbital bombardment: ship -> colony ---
  // Verifies that bombardment:
  //  - sets diplomacy to hostile
  //  - reduces ground forces first
  //  - then destroys installations (HP scales with construction_cost)
  //  - respects duration_days by removing the order
  {
    nebula4x::ContentDB content;

    // Target installation definition.
    nebula4x::InstallationDef bunker;
    bunker.id = "bunker";
    bunker.name = "Bunker";
    bunker.construction_cost = 100;
    content.installations[bunker.id] = bunker;

    // Bomber design.
    nebula4x::ShipDesign bomber;
    bomber.id = "bomber_beta";
    bomber.name = "Bomber Beta";
    bomber.max_hp = 10.0;
    bomber.speed_km_s = 0.0;
    bomber.weapon_damage = 10.0;
    bomber.weapon_range_mkm = 20.0;
    // Avoid power gating in tests.
    bomber.power_generation = 0.0;
    bomber.power_use_weapons = 0.0;
    content.designs[bomber.id] = bomber;

    nebula4x::SimConfig cfg;
    cfg.bombard_ground_strength_per_damage = 1.0;
    cfg.bombard_installation_hp_per_construction_cost = 0.02;
    cfg.bombard_population_millions_per_damage = 0.05;

    nebula4x::Simulation sim(std::move(content), cfg);

    // Custom minimal scenario.
    nebula4x::GameState s;
    const nebula4x::Id kFacA = 1;
    const nebula4x::Id kFacB = 2;
    const nebula4x::Id kSys0 = 100;
    const nebula4x::Id kBody0 = 200;
    const nebula4x::Id kCol0 = 300;
    const nebula4x::Id kShip0 = 400;

    {
      nebula4x::Faction f;
      f.id = kFacA;
      f.name = "A";
      s.factions[f.id] = f;
    }
    {
      nebula4x::Faction f;
      f.id = kFacB;
      f.name = "B";
      s.factions[f.id] = f;
    }
    {
      nebula4x::StarSystem sys;
      sys.id = kSys0;
      sys.name = "TestSys";
      sys.bodies.push_back(kBody0);
      s.systems[sys.id] = sys;
    }
    {
      nebula4x::Body body;
      body.id = kBody0;
      body.name = "TestPlanet";
      body.system_id = kSys0;
      body.position_mkm = {0.0, 0.0};
      s.bodies[body.id] = body;
    }
    {
      nebula4x::Colony col;
      col.id = kCol0;
      col.name = "Target";
      col.faction_id = kFacB;
      col.body_id = kBody0;
      col.population_millions = 10.0;
      col.ground_forces = 15.0;
      col.installations["bunker"] = 3;
      s.colonies[col.id] = col;
    }
    {
      nebula4x::Ship sh;
      sh.id = kShip0;
      sh.name = "Bomber";
      sh.faction_id = kFacA;
      sh.system_id = kSys0;
      sh.design_id = "bomber_beta";
      sh.position_mkm = {10.0, 0.0};
      sh.hp = 10.0;
      s.ships[sh.id] = sh;
    }

    sim.load_game(std::move(s));
    N4X_ASSERT(sim.issue_bombard_colony(kShip0, kCol0, /*duration_days=*/2, /*use_fog_of_war=*/false));

    sim.advance_days(1);
    {
      const auto& col = sim.state().colonies.at(kCol0);
      N4X_ASSERT(std::abs(col.ground_forces - 5.0) < 1e-9);
      N4X_ASSERT(col.installations.at("bunker") == 3);
      N4X_ASSERT(sim.are_factions_hostile(kFacA, kFacB));
    }

    sim.advance_days(1);
    {
      const auto& col = sim.state().colonies.at(kCol0);
      N4X_ASSERT(col.ground_forces <= 1e-9);
      N4X_ASSERT(col.installations.at("bunker") == 1);
      N4X_ASSERT(col.population_millions < 10.0);
    }

    {
      const auto it = sim.state().ship_orders.find(kShip0);
      N4X_ASSERT(it != sim.state().ship_orders.end());
      N4X_ASSERT(it->second.queue.empty());
    }
  }


  // --- auto-freight should honor colony stockpile targets ---
  {
    nebula4x::ContentDB content;

    // Minimal ship designs required by the Sol scenario.
    {
      nebula4x::ShipDesign freighter;
      freighter.id = "freighter_alpha";
      freighter.name = "Freighter Alpha";
      freighter.cargo_tons = 10000.0;
      freighter.speed_km_s = 1000.0;      // fast enough to reach Mars within a day
      freighter.fuel_use_per_mkm = 0.0;   // simplify: no fuel gating in this test
      content.designs[freighter.id] = freighter;

      nebula4x::ShipDesign surveyor;
      surveyor.id = "surveyor_beta";
      surveyor.name = "Surveyor Beta";
      surveyor.speed_km_s = 1000.0;
      content.designs[surveyor.id] = surveyor;

      nebula4x::ShipDesign escort;
      escort.id = "escort_gamma";
      escort.name = "Escort Gamma";
      escort.speed_km_s = 1000.0;
      content.designs[escort.id] = escort;
    }

    nebula4x::SimConfig cfg;
    cfg.auto_freight_industry_input_buffer_days = 0.0;  // keep the test focused on targets
    cfg.enable_combat = false;

    nebula4x::Simulation sim(std::move(content), cfg);

    const auto earth_id = find_colony_id(sim.state(), "Earth");
    const auto mars_id = find_colony_id(sim.state(), "Mars Outpost");
    N4X_ASSERT(earth_id != nebula4x::kInvalidId);
    N4X_ASSERT(mars_id != nebula4x::kInvalidId);

    const auto freighter_id = find_ship_id(sim.state(), "Freighter Alpha");
    N4X_ASSERT(freighter_id != nebula4x::kInvalidId);

    // Mars starts with 1000 Fuel in the Sol scenario. Target 5000 should require import.
    sim.state().colonies[mars_id].mineral_targets["Fuel"] = 5000.0;

    // Enable automation and ensure we're idle.
    sim.state().ships[freighter_id].auto_freight = true;
    sim.state().ship_orders[freighter_id].queue.clear();
    sim.state().ships[freighter_id].cargo.clear();

    auto fuel_at = [](const nebula4x::Colony& c) {
      auto it = c.minerals.find("Fuel");
      return (it == c.minerals.end()) ? 0.0 : it->second;
    };

    const double fuel0 = fuel_at(sim.state().colonies.at(mars_id));
    N4X_ASSERT(fuel0 <= 1000.0 + 1e-6);

    // Day 1: auto-freight assigns orders and loads at Earth.
    // Day 2: ship moves to Mars and unloads.
    sim.advance_days(2);

    const double fuel1 = fuel_at(sim.state().colonies.at(mars_id));
    N4X_ASSERT(fuel1 >= 5000.0 - 1e-6);
  }

  // --- stealth signature reduces detection range ---
  {
    nebula4x::ContentDB content;

    // Minimal designs for sensor source + targets.
    nebula4x::ShipDesign sensor;
    sensor.id = "sensor_ship";
    sensor.name = "Sensor Ship";
    sensor.mass_tons = 100.0;
    sensor.max_hp = 100.0;
    sensor.sensor_range_mkm = 10.0;
    sensor.signature_multiplier = 1.0;
    content.designs[sensor.id] = sensor;

    nebula4x::ShipDesign normal;
    normal.id = "target_normal";
    normal.name = "Target Normal";
    normal.mass_tons = 100.0;
    normal.max_hp = 100.0;
    normal.sensor_range_mkm = 0.0;
    normal.signature_multiplier = 1.0;
    content.designs[normal.id] = normal;

    nebula4x::ShipDesign stealth = normal;
    stealth.id = "target_stealth";
    stealth.name = "Target Stealth";
    stealth.signature_multiplier = 0.5;
    content.designs[stealth.id] = stealth;

    nebula4x::SimConfig cfg;
    cfg.enable_combat = false;

    nebula4x::Simulation sim(std::move(content), cfg);
    auto& st = sim.state();

    // Add a dedicated test system + factions to keep the check isolated.
    const auto sys_id = nebula4x::allocate_id(st);
    nebula4x::StarSystem sys;
    sys.id = sys_id;
    sys.name = "Test System";
    st.systems[sys_id] = sys;

    const auto f1 = nebula4x::allocate_id(st);
    nebula4x::Faction fac1;
    fac1.id = f1;
    fac1.name = "Alpha";
    st.factions[f1] = fac1;

    const auto f2 = nebula4x::allocate_id(st);
    nebula4x::Faction fac2;
    fac2.id = f2;
    fac2.name = "Beta";
    st.factions[f2] = fac2;

    auto add_ship = [&](nebula4x::Id fid, const std::string& name, const std::string& design_id, double x,
                        double y) {
      const auto sid = nebula4x::allocate_id(st);
      nebula4x::Ship sh;
      sh.id = sid;
      sh.name = name;
      sh.faction_id = fid;
      sh.system_id = sys_id;
      sh.design_id = design_id;
      sh.position_mkm = nebula4x::Vec2{x, y};
      sh.hp = 100.0;
      st.ships[sid] = sh;
      st.systems[sys_id].ships.push_back(sid);
      return sid;
    };

    (void)add_ship(f1, "Sensor", "sensor_ship", 0.0, 0.0);
    const auto normal_id = add_ship(f2, "Normal", "target_normal", 7.0, 0.0);
    const auto stealth_id = add_ship(f2, "Stealth", "target_stealth", 7.0, 1.0);

    // Base sensor range is 10 mkm. Stealth signature 0.5 should cut that to 5 mkm.
    N4X_ASSERT(sim.is_ship_detected_by_faction(f1, normal_id));
    N4X_ASSERT(!sim.is_ship_detected_by_faction(f1, stealth_id));

    // Also verify the contact tick respects signature.
    sim.advance_days(1);
    N4X_ASSERT(st.factions[f1].ship_contacts.count(normal_id) == 1);
    N4X_ASSERT(st.factions[f1].ship_contacts.count(stealth_id) == 0);
  }

  // --- EMCON / sensor mode affects detection (range + signature) ---
  {
    nebula4x::ContentDB content;

    nebula4x::ShipDesign sensor;
    sensor.id = "emcon_sensor";
    sensor.name = "EMCON Sensor";
    sensor.mass_tons = 100.0;
    sensor.max_hp = 100.0;
    sensor.sensor_range_mkm = 10.0;
    sensor.signature_multiplier = 1.0;
    content.designs[sensor.id] = sensor;

    nebula4x::ShipDesign target;
    target.id = "emcon_target";
    target.name = "EMCON Target";
    target.mass_tons = 100.0;
    target.max_hp = 100.0;
    // Must have sensors for SensorMode signature effects to apply.
    target.sensor_range_mkm = 5.0;
    target.signature_multiplier = 1.0;
    content.designs[target.id] = target;

    nebula4x::SimConfig cfg;
    cfg.enable_combat = false;
    cfg.sensor_mode_active_range_multiplier = 2.0;
    cfg.sensor_mode_active_signature_multiplier = 2.0;
    cfg.sensor_mode_passive_range_multiplier = 0.5;
    cfg.sensor_mode_passive_signature_multiplier = 0.5;

    nebula4x::Simulation sim(std::move(content), cfg);
    auto& st = sim.state();

    // Isolated test system + factions.
    const auto sys_id = nebula4x::allocate_id(st);
    nebula4x::StarSystem sys;
    sys.id = sys_id;
    sys.name = "EMCON Test System";
    st.systems[sys_id] = sys;

    const auto f1 = nebula4x::allocate_id(st);
    nebula4x::Faction fac1;
    fac1.id = f1;
    fac1.name = "Viewer";
    st.factions[f1] = fac1;

    const auto f2 = nebula4x::allocate_id(st);
    nebula4x::Faction fac2;
    fac2.id = f2;
    fac2.name = "Target";
    st.factions[f2] = fac2;

    auto add_ship = [&](nebula4x::Id fid, const std::string& name, const std::string& design_id, double x,
                        double y) {
      const auto sid = nebula4x::allocate_id(st);
      nebula4x::Ship sh;
      sh.id = sid;
      sh.name = name;
      sh.faction_id = fid;
      sh.system_id = sys_id;
      sh.design_id = design_id;
      sh.position_mkm = nebula4x::Vec2{x, y};
      sh.hp = 100.0;
      st.ships[sid] = sh;
      st.systems[sys_id].ships.push_back(sid);
      return sid;
    };

    const auto sensor_id = add_ship(f1, "Sensor", "emcon_sensor", 0.0, 0.0);
    const auto target_id = add_ship(f2, "Target", "emcon_target", 12.0, 0.0);

    // Base sensor range is 10 mkm. Target at 12 should be out of range.
    N4X_ASSERT(!sim.is_ship_detected_by_faction(f1, target_id));
    sim.advance_days(1);
    N4X_ASSERT(st.factions[f1].ship_contacts.count(target_id) == 0);

    // Active sensors on the *viewer* should extend sensor coverage.
    st.ships[sensor_id].sensor_mode = nebula4x::SensorMode::Active;
    st.factions[f1].ship_contacts.clear();
    N4X_ASSERT(sim.is_ship_detected_by_faction(f1, target_id));
    sim.advance_days(1);
    N4X_ASSERT(st.factions[f1].ship_contacts.count(target_id) == 1);

    // Active sensors on the *target* should increase detectability (signature > 1).
    st.ships[sensor_id].sensor_mode = nebula4x::SensorMode::Normal;
    st.ships[target_id].sensor_mode = nebula4x::SensorMode::Active;
    st.factions[f1].ship_contacts.clear();
    N4X_ASSERT(sim.is_ship_detected_by_faction(f1, target_id));
    sim.advance_days(1);
    N4X_ASSERT(st.factions[f1].ship_contacts.count(target_id) == 1);

    // Passive sensors should reduce detectability enough to hide even inside the nominal range.
    st.ships[target_id].position_mkm = nebula4x::Vec2{7.0, 0.0};
    st.ships[target_id].sensor_mode = nebula4x::SensorMode::Passive;
    st.factions[f1].ship_contacts.clear();
    N4X_ASSERT(!sim.is_ship_detected_by_faction(f1, target_id));
    sim.advance_days(1);
    N4X_ASSERT(st.factions[f1].ship_contacts.count(target_id) == 0);
  }


  {
    // Installation targets (auto-build) should enqueue, prune, and complete construction.
    auto content = nebula4x::load_content_db_from_file("data/blueprints/starting_blueprints.json");
    content.techs = nebula4x::load_tech_db_from_file("data/tech/tech_tree.json");

    nebula4x::Simulation sim(std::move(content), nebula4x::SimConfig{});

    const auto earth_id = find_colony_id(sim.state(), "Earth");
    N4X_ASSERT(earth_id != nebula4x::kInvalidId);

    auto fortress_count = [](const nebula4x::Colony& c) {
      auto it = c.installations.find("planetary_fortress");
      return (it == c.installations.end()) ? 0 : it->second;
    };

    // Keep this test deterministic: clear any existing construction orders.
    sim.state().colonies[earth_id].construction_queue.clear();

    const int start = fortress_count(sim.state().colonies.at(earth_id));

    // Request +3, let the sim auto-queue and start building.
    sim.state().colonies[earth_id].installation_targets["planetary_fortress"] = start + 3;
    sim.advance_days(1);

    int queued_auto = 0;
    for (const auto& o : sim.state().colonies.at(earth_id).construction_queue) {
      if (o.installation_id == "planetary_fortress") {
        N4X_ASSERT(o.auto_queued);
        queued_auto += std::max(0, o.quantity_remaining);
      }
    }
    N4X_ASSERT(queued_auto >= 3);

    // Now lower the target to only +1 and ensure pending auto units are pruned.
    sim.state().colonies[earth_id].installation_targets["planetary_fortress"] = start + 1;

    // Plenty of time for one fortress to complete (600 CP @ ~335 CP/day).
    sim.advance_days(20);

    N4X_ASSERT(fortress_count(sim.state().colonies.at(earth_id)) == start + 1);

    // No remaining auto orders for this installation id.
    for (const auto& o : sim.state().colonies.at(earth_id).construction_queue) {
      N4X_ASSERT(!(o.auto_queued && o.installation_id == "planetary_fortress"));
    }
  }

  {

    // Wreck salvage (order + tick)
    using namespace nebula4x;
    ContentDB content;

    // Provide a shipyard proxy so hull salvage has deterministic mineral weights
    // (even though this test manually creates a wreck).
    InstallationDef yard;
    yard.id = "shipyard";
    yard.name = "Shipyard";
    yard.build_costs_per_ton["Duranium"] = 2.0;
    yard.build_costs_per_ton["Neutronium"] = 1.0;
    content.installations[yard.id] = yard;

    ShipDesign salvager;
    salvager.id = "salvager";
    salvager.name = "Salvager";
    salvager.mass_tons = 100.0;
    salvager.speed_km_s = 0.0;
    salvager.max_fuel_tons = 0.0;
    salvager.max_hp = 10.0;
    salvager.cargo_tons = 1000.0;
    content.designs[salvager.id] = salvager;

    SimConfig cfg;
    cfg.enable_combat = false;

    Simulation sim(std::move(content), cfg);
    GameState st;

    // Minimal single-system state.
    const auto sys_id = allocate_id(st);
    StarSystem sys;
    sys.id = sys_id;
    sys.name = "Test System";
    st.systems[sys_id] = sys;

    const auto fac_id = allocate_id(st);
    Faction fac;
    fac.id = fac_id;
    fac.name = "Player";
    fac.discovered_systems.insert(sys_id);
    st.factions[fac_id] = fac;

    const auto ship_id = allocate_id(st);
    Ship sh;
    sh.id = ship_id;
    sh.name = "Salvager-1";
    sh.faction_id = fac_id;
    sh.system_id = sys_id;
    sh.design_id = "salvager";
    sh.position_mkm = Vec2{0.0, 0.0};
    sh.hp = 10.0;
    st.ships[ship_id] = sh;
    st.systems[sys_id].ships.push_back(ship_id);

    // A wreck at the same location.
    const auto wreck_id = allocate_id(st);
    Wreck w;
    w.id = wreck_id;
    w.name = "Wreck: Target";
    w.system_id = sys_id;
    w.position_mkm = Vec2{0.0, 0.0};
    w.minerals["Duranium"] = 50.0;
    w.created_day = 0;
    st.wrecks[wreck_id] = w;

    sim.load_game(st);

    // Salvage all (tons=0) should take everything and remove the wreck.
    N4X_ASSERT(sim.issue_salvage_wreck(ship_id, wreck_id, "", 0.0, false));
    sim.tick_ships();
    N4X_ASSERT(sim.state().ships.at(ship_id).cargo.at("Duranium") == 50.0);
    N4X_ASSERT(sim.state().wrecks.empty());
  }

  // --- escort order sanity check (same system) ---
  // The escort should approach the target and stop at the requested separation.
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

    nebula4x::Simulation sim(std::move(content), nebula4x::SimConfig{});

    const auto escort_id = find_ship_id(sim.state(), "Escort Gamma");
    const auto target_id = find_ship_id(sim.state(), "Surveyor Beta");
    N4X_ASSERT(escort_id != nebula4x::kInvalidId);
    N4X_ASSERT(target_id != nebula4x::kInvalidId);

    auto* escort = nebula4x::find_ptr(sim.state().ships, escort_id);
    auto* target = nebula4x::find_ptr(sim.state().ships, target_id);
    N4X_ASSERT(escort);
    N4X_ASSERT(target);

    // Same system; target stationary.
    target->system_id = escort->system_id;
    escort->position_mkm = {0.0, 0.0};
    target->position_mkm = {10.0, 0.0};

    // Make sure the escort can move.
    escort->speed_km_s = 200.0;
    target->speed_km_s = 0.0;

    const double follow = 1.5;
    N4X_ASSERT(sim.issue_escort_ship(escort_id, target_id, follow));
    sim.advance_days(1);

    const auto* escort_after = nebula4x::find_ptr(sim.state().ships, escort_id);
    const auto* target_after = nebula4x::find_ptr(sim.state().ships, target_id);
    N4X_ASSERT(escort_after);
    N4X_ASSERT(target_after);

    const double dist = (escort_after->position_mkm - target_after->position_mkm).length();
    N4X_ASSERT(std::abs(dist - follow) < 1e-6);

    const auto it_orders = sim.state().ship_orders.find(escort_id);
    N4X_ASSERT(it_orders != sim.state().ship_orders.end());
    N4X_ASSERT(!it_orders->second.queue.empty());
    N4X_ASSERT(std::holds_alternative<nebula4x::EscortShip>(it_orders->second.queue.front()));
  }

  // --- escort order sanity check (cross-system jump transit) ---
  // If the target is in another system and the escort is at the jump point,
  // it should transit while keeping the EscortShip order.
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

    nebula4x::SimConfig cfg;
    cfg.fleet_coordinated_jumps = false;
    nebula4x::Simulation sim(std::move(content), cfg);

    const auto escort_id = find_ship_id(sim.state(), "Escort Gamma");
    const auto target_id = find_ship_id(sim.state(), "Surveyor Beta");
    const auto sol_id = find_system_id(sim.state(), "Sol");
    const auto centauri_id = find_system_id(sim.state(), "Alpha Centauri");
    N4X_ASSERT(escort_id != nebula4x::kInvalidId);
    N4X_ASSERT(target_id != nebula4x::kInvalidId);
    N4X_ASSERT(sol_id != nebula4x::kInvalidId);
    N4X_ASSERT(centauri_id != nebula4x::kInvalidId);

    auto* escort = nebula4x::find_ptr(sim.state().ships, escort_id);
    auto* target = nebula4x::find_ptr(sim.state().ships, target_id);
    N4X_ASSERT(escort);
    N4X_ASSERT(target);

    // Move the target into Alpha Centauri (and fix the systems' ship lists).
    if (escort->system_id != sol_id) escort->system_id = sol_id;
    if (target->system_id != sol_id) target->system_id = sol_id;

    auto& sol_ships = sim.state().systems.at(sol_id).ships;
    auto& cen_ships = sim.state().systems.at(centauri_id).ships;
    sol_ships.erase(std::remove(sol_ships.begin(), sol_ships.end(), target_id), sol_ships.end());
    cen_ships.push_back(target_id);
    target->system_id = centauri_id;
    target->position_mkm = {80.0, 0.0};

    // Find the Sol->Alpha Centauri jump point and place the escort on it.
    nebula4x::Id sol_to_cen_jp = nebula4x::kInvalidId;
    for (const auto& [jid, jp] : sim.state().jump_points) {
      if (jp.system_id != sol_id) continue;
      const auto* linked = nebula4x::find_ptr(sim.state().jump_points, jp.linked_jump_id);
      if (linked && linked->system_id == centauri_id) {
        sol_to_cen_jp = jid;
        break;
      }
    }
    N4X_ASSERT(sol_to_cen_jp != nebula4x::kInvalidId);
    const auto& jp = sim.state().jump_points.at(sol_to_cen_jp);
    escort->system_id = sol_id;
    escort->position_mkm = jp.position_mkm;

    N4X_ASSERT(sim.issue_escort_ship(escort_id, target_id, 1.0));
    sim.advance_days(1);

    const auto* escort_after = nebula4x::find_ptr(sim.state().ships, escort_id);
    N4X_ASSERT(escort_after);
    N4X_ASSERT(escort_after->system_id == centauri_id);

    const auto it_orders = sim.state().ship_orders.find(escort_id);
    N4X_ASSERT(it_orders != sim.state().ship_orders.end());
    N4X_ASSERT(!it_orders->second.queue.empty());
    N4X_ASSERT(std::holds_alternative<nebula4x::EscortShip>(it_orders->second.queue.front()));
  }


  {
    // Ship design targets (auto-shipyards).
    nebula4x::ContentDB content;
    content.installations["shipyard"] = nebula4x::InstallationDef{.id = "shipyard",
                                                                  .name = "Shipyard",
                                                                  .build_rate_tons_per_day = 50.0};

    nebula4x::ShipDesign d;
    d.id = "test_ship";
    d.name = "Test Ship";
    d.mass_tons = 200.0;
    d.max_hp = 100.0;
    content.designs[d.id] = d;

    nebula4x::Simulation sim(std::move(content), {});

    nebula4x::GameState s;
    s.save_version = 35;

    nebula4x::StarSystem sys;
    sys.id = 1;
    sys.name = "TestSys";
    s.systems[sys.id] = sys;

    nebula4x::Body body;
    body.id = 1;
    body.system_id = sys.id;
    body.name = "TestBody";
    body.type = "planet";
    body.position_mkm = nebula4x::Vec2{0.0, 0.0};
    s.bodies[body.id] = body;

    nebula4x::Faction f;
    f.id = 1;
    f.name = "TestFaction";
    f.ship_design_targets["test_ship"] = 1;
    s.factions[f.id] = f;

    nebula4x::Colony c;
    c.id = 1;
    c.name = "TestColony";
    c.faction_id = f.id;
    c.system_id = sys.id;
    c.body_id = body.id;
    c.installations["shipyard"] = 1;
    s.colonies[c.id] = c;

    sim.load_game(std::move(s));

    // First tick should auto-queue the build order (and progress it).
    sim.tick_shipyards();
    auto& st = sim.state();
    auto& c2 = st.colonies.at(1);
    N4X_ASSERT(!c2.shipyard_queue.empty());
    N4X_ASSERT(c2.shipyard_queue.front().design_id == "test_ship");
    N4X_ASSERT(c2.shipyard_queue.front().auto_queued);

    // Run until the ship completes.
    for (int i = 0; i < 10; ++i) sim.tick_shipyards();
    N4X_ASSERT(st.ships.size() == 1);
    N4X_ASSERT(st.ships.begin()->second.design_id == "test_ship");

    // Ensure no extra auto orders once the target is met.
    sim.tick_shipyards();
    N4X_ASSERT(st.colonies.at(1).shipyard_queue.empty());
  }
  return 0;
}
