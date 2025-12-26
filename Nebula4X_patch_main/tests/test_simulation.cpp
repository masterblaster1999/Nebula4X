#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>
#include <utility>
#include <variant>

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

  return 0;
}
