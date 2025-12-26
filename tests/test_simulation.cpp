#include <cmath>
#include <iostream>

#include "nebula4x/core/simulation.h"

#define N4X_ASSERT(expr) \
  do { \
    if (!(expr)) { \
      std::cerr << "ASSERT failed: " #expr " (" << __FILE__ << ":" << __LINE__ << ")\n"; \
      return 1; \
    } \
  } while (0)

int test_simulation() {
  // --- mineral production sanity check ---
  // Minimal content DB for installations.
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

  // Minimal design.
  nebula4x::ShipDesign d;
  d.id = "freighter_alpha";
  d.name = "Freighter Alpha";
  d.mass_tons = 100;
  d.speed_km_s = 10;
  content.designs[d.id] = d;

  nebula4x::Simulation sim(std::move(content), nebula4x::SimConfig{});

  auto before = sim.state().colonies.begin()->second.minerals["Duranium"];
  sim.advance_days(2);
  auto after = sim.state().colonies.begin()->second.minerals["Duranium"];

  N4X_ASSERT(after > before);

  // --- shipyard build costs sanity check ---
  // When build_costs_per_ton are configured for the shipyard, advancing time with an
  // active build order should consume minerals.
  nebula4x::ContentDB content2;

  nebula4x::InstallationDef yard2;
  yard2.id = "shipyard";
  yard2.name = "Shipyard";
  yard2.build_rate_tons_per_day = 50;
  yard2.build_costs_per_ton = {{"Duranium", 1.0}}; // 1 mineral per ton
  content2.installations[yard2.id] = yard2;

  // No mineral production to keep the check simple.
  nebula4x::InstallationDef mine2;
  mine2.id = "automated_mine";
  mine2.name = "Automated Mine";
  mine2.produces_per_day = {{"Duranium", 0.0}};
  content2.installations[mine2.id] = mine2;

  // Minimal design.
  nebula4x::ShipDesign d2;
  d2.id = "freighter_alpha";
  d2.name = "Freighter Alpha";
  d2.mass_tons = 100;
  d2.speed_km_s = 10;
  content2.designs[d2.id] = d2;

  nebula4x::Simulation sim2(std::move(content2), nebula4x::SimConfig{});

  const auto colony_id = sim2.state().colonies.begin()->first;
  const double before_build = sim2.state().colonies.begin()->second.minerals["Duranium"];
  N4X_ASSERT(sim2.enqueue_build(colony_id, "freighter_alpha"));
  sim2.advance_days(1);
  const double after_build = sim2.state().colonies.begin()->second.minerals["Duranium"];
  N4X_ASSERT(after_build < before_build);

  // --- colony construction queue sanity check ---
  // Enqueuing an installation build should consume minerals and increase
  // installation count once enough construction points are available.
  nebula4x::ContentDB content3;

  nebula4x::InstallationDef mine3;
  mine3.id = "automated_mine";
  mine3.name = "Automated Mine";
  mine3.produces_per_day = {{"Duranium", 0.0}}; // avoid production influencing the check
  mine3.construction_cost = 50.0;
  mine3.build_costs = {{"Duranium", 100.0}};
  content3.installations[mine3.id] = mine3;

  nebula4x::InstallationDef yard3;
  yard3.id = "shipyard";
  yard3.name = "Shipyard";
  yard3.build_rate_tons_per_day = 0.0;
  content3.installations[yard3.id] = yard3;

  // Keep designs minimal; none are needed for this test.
  nebula4x::Simulation sim3(std::move(content3), nebula4x::SimConfig{});
  const auto colony3_id = sim3.state().colonies.begin()->first;
  auto& col3 = sim3.state().colonies.begin()->second;

  const int mines_before = col3.installations["automated_mine"];
  const double dur_before = col3.minerals["Duranium"];

  N4X_ASSERT(sim3.enqueue_installation_build(colony3_id, "automated_mine", 1));
  sim3.advance_days(1);

  const int mines_after = sim3.state().colonies.begin()->second.installations["automated_mine"];
  const double dur_after = sim3.state().colonies.begin()->second.minerals["Duranium"];

  N4X_ASSERT(mines_after == mines_before + 1);
  N4X_ASSERT(dur_after < dur_before);


  // --- sensor detection sanity check ---
  // Hostile ships should only be detected when within sensor range of some friendly sensor source.
  nebula4x::ContentDB content4;

  // Minimal installations to satisfy scenario content references.
  nebula4x::InstallationDef mine4;
  mine4.id = "automated_mine";
  mine4.name = "Automated Mine";
  mine4.produces_per_day = { {"Duranium", 0.0} };
  content4.installations[mine4.id] = mine4;

  nebula4x::InstallationDef yard4;
  yard4.id = "shipyard";
  yard4.name = "Shipyard";
  yard4.build_rate_tons_per_day = 0.0;
  content4.installations[yard4.id] = yard4;

  // Designs used by the default Sol scenario.
  auto make_design = [](const std::string& id, double sensor_range_mkm) {
    nebula4x::ShipDesign d;
    d.id = id;
    d.name = id;
    d.max_hp = 10.0;
    d.speed_km_s = 0.0;
    d.sensor_range_mkm = sensor_range_mkm;
    return d;
  };

  content4.designs["freighter_alpha"] = make_design("freighter_alpha", 0.0);
  content4.designs["surveyor_beta"]  = make_design("surveyor_beta", 100.0);
  content4.designs["escort_gamma"]   = make_design("escort_gamma", 100.0);
  content4.designs["pirate_raider"]  = make_design("pirate_raider", 0.0);

  nebula4x::Simulation sim4(std::move(content4), nebula4x::SimConfig{});

  nebula4x::Id terrans_id = nebula4x::kInvalidId;
  nebula4x::Id pirates_id = nebula4x::kInvalidId;
  for (const auto& [fid, f] : sim4.state().factions) {
    if (f.name == "Terran Union") terrans_id = fid;
    if (f.name == "Pirate Raiders") pirates_id = fid;
  }
  N4X_ASSERT(terrans_id != nebula4x::kInvalidId);
  N4X_ASSERT(pirates_id != nebula4x::kInvalidId);

  nebula4x::Id sol_id = nebula4x::kInvalidId;
  for (const auto& [sid, sys] : sim4.state().systems) {
    if (sys.name == "Sol") sol_id = sid;
  }
  N4X_ASSERT(sol_id != nebula4x::kInvalidId);

  const auto* sol = nebula4x::find_ptr(sim4.state().systems, sol_id);
  N4X_ASSERT(sol);

  // Use the first Terran ship as a reference point.
  nebula4x::Vec2 terran_pos{0.0, 0.0};
  bool found_terran_ship = false;
  for (nebula4x::Id sid : sol->ships) {
    const auto* sh = nebula4x::find_ptr(sim4.state().ships, sid);
    if (sh && sh->faction_id == terrans_id) {
      terran_pos = sh->position_mkm;
      found_terran_ship = true;
      break;
    }
  }
  N4X_ASSERT(found_terran_ship);

  // Spawn a pirate ship within 100 mkm (detected), then move it out of range (not detected).
  const nebula4x::Id pirate_ship_id = nebula4x::allocate_id(sim4.state());
  {
    nebula4x::Ship p;
    p.id = pirate_ship_id;
    p.name = "Test Raider";
    p.faction_id = pirates_id;
    p.system_id = sol_id;
    p.design_id = "pirate_raider";
    p.position_mkm = terran_pos + nebula4x::Vec2{0.0, 50.0};
    p.hp = 10.0;

    sim4.state().ships[p.id] = p;
    sim4.state().ship_orders[p.id] = nebula4x::ShipOrders{};
    sim4.state().systems[sol_id].ships.push_back(p.id);
  }

  N4X_ASSERT(sim4.is_ship_detected_by_faction(terrans_id, pirate_ship_id));

  sim4.state().ships[pirate_ship_id].position_mkm = terran_pos + nebula4x::Vec2{0.0, 500.0};
  N4X_ASSERT(!sim4.is_ship_detected_by_faction(terrans_id, pirate_ship_id));

  // --- contact memory sanity check ---
  // If a hostile ship is detected, the viewer faction should store a contact snapshot
  // that remains for some time even after contact is lost.
  nebula4x::ContentDB content5;

  // Minimal installations (the scenario references these ids).
  content5.installations[mine4.id] = mine4;
  content5.installations[yard4.id] = yard4;

  // Designs with Terran ship sensors.
  content5.designs["freighter_alpha"] = make_design("freighter_alpha", 0.0);
  content5.designs["surveyor_beta"]  = make_design("surveyor_beta", 100.0);
  content5.designs["escort_gamma"]   = make_design("escort_gamma", 100.0);
  content5.designs["pirate_raider"]  = make_design("pirate_raider", 0.0);

  nebula4x::Simulation sim5(std::move(content5), nebula4x::SimConfig{});

  nebula4x::Id terrans5 = nebula4x::kInvalidId;
  nebula4x::Id pirates5 = nebula4x::kInvalidId;
  for (const auto& [fid, f] : sim5.state().factions) {
    if (f.name == "Terran Union") terrans5 = fid;
    if (f.name == "Pirate Raiders") pirates5 = fid;
  }
  N4X_ASSERT(terrans5 != nebula4x::kInvalidId);
  N4X_ASSERT(pirates5 != nebula4x::kInvalidId);

  nebula4x::Id sol5 = nebula4x::kInvalidId;
  for (const auto& [sid, sys] : sim5.state().systems) {
    if (sys.name == "Sol") sol5 = sid;
  }
  N4X_ASSERT(sol5 != nebula4x::kInvalidId);

  const auto* sol_sys = nebula4x::find_ptr(sim5.state().systems, sol5);
  N4X_ASSERT(sol_sys);

  // Find a Terran ship (we'll use it as an attacker for later intercept checks).
  nebula4x::Id terran_ship5 = nebula4x::kInvalidId;
  nebula4x::Vec2 terran_pos5{0.0, 0.0};
  bool found_terran_ship5 = false;
  for (nebula4x::Id sid : sol_sys->ships) {
    const auto* sh = nebula4x::find_ptr(sim5.state().ships, sid);
    if (sh && sh->faction_id == terrans5) {
      terran_pos5 = sh->position_mkm;
      terran_ship5 = sid;
      found_terran_ship5 = true;
      break;
    }
  }
  N4X_ASSERT(found_terran_ship5);
  N4X_ASSERT(terran_ship5 != nebula4x::kInvalidId);

  // Spawn a pirate ship within detection range.
  const nebula4x::Id pirate_contact_id = nebula4x::allocate_id(sim5.state());
  {
    nebula4x::Ship p;
    p.id = pirate_contact_id;
    p.name = "Contact Raider";
    p.faction_id = pirates5;
    p.system_id = sol5;
    p.design_id = "pirate_raider";
    p.position_mkm = terran_pos5 + nebula4x::Vec2{0.0, 50.0};
    p.hp = 10.0;
    sim5.state().ships[p.id] = p;
    sim5.state().ship_orders[p.id] = nebula4x::ShipOrders{};
    sim5.state().systems[sol5].ships.push_back(p.id);
  }

  // Advance one day so contacts tick.
  sim5.advance_days(1);
  N4X_ASSERT(sim5.is_ship_detected_by_faction(terrans5, pirate_contact_id));
  N4X_ASSERT(sim5.state().factions[terrans5].ship_contacts.count(pirate_contact_id) == 1);

  // Move out of range and advance; contact should remain but detection should be false.
  sim5.state().ships[pirate_contact_id].position_mkm = terran_pos5 + nebula4x::Vec2{0.0, 500.0};
  sim5.advance_days(1);
  N4X_ASSERT(!sim5.is_ship_detected_by_faction(terrans5, pirate_contact_id));
  N4X_ASSERT(sim5.state().factions[terrans5].ship_contacts.count(pirate_contact_id) == 1);

  const auto recent = sim5.recent_contacts_in_system(terrans5, sol5, 30);
  N4X_ASSERT(!recent.empty());

  // --- contact-based intercept / attack order sanity check ---
  // A faction should be able to issue an AttackShip order against a target that is
  // not currently detected, as long as it has a stored contact snapshot in the same system.
  {
    const auto& contact = sim5.state().factions[terrans5].ship_contacts.at(pirate_contact_id);
    N4X_ASSERT(!sim5.is_ship_detected_by_faction(terrans5, pirate_contact_id));
    N4X_ASSERT(sim5.issue_attack_ship(terran_ship5, pirate_contact_id));

    const auto& q = sim5.state().ship_orders.at(terran_ship5).queue;
    N4X_ASSERT(!q.empty());
    N4X_ASSERT(std::holds_alternative<nebula4x::AttackShip>(q.back()));
    const auto& ord = std::get<nebula4x::AttackShip>(q.back());
    N4X_ASSERT(ord.target_ship_id == pirate_contact_id);
    N4X_ASSERT(ord.has_last_known);
    N4X_ASSERT(std::abs(ord.last_known_position_mkm.x - contact.last_seen_position_mkm.x) < 1e-6);
    N4X_ASSERT(std::abs(ord.last_known_position_mkm.y - contact.last_seen_position_mkm.y) < 1e-6);
  }


  // --- exploration discovery sanity check ---
  // Factions track which star systems they have discovered.
  // Starting discovery is seeded from existing colonies/ships, and traveling
  // via a jump point should discover the destination system.
  nebula4x::ContentDB content6;

  // Minimal installations (the scenario references these ids).
  content6.installations[mine4.id] = mine4;
  content6.installations[yard4.id] = yard4;

  // Designs used by the default Sol scenario. Speed is irrelevant for the
  // "already at jump point" transit check.
  content6.designs["freighter_alpha"] = make_design("freighter_alpha", 0.0);
  content6.designs["surveyor_beta"]  = make_design("surveyor_beta", 0.0);
  content6.designs["escort_gamma"]   = make_design("escort_gamma", 0.0);
  content6.designs["pirate_raider"]  = make_design("pirate_raider", 0.0);

  nebula4x::Simulation sim6(std::move(content6), nebula4x::SimConfig{});

  nebula4x::Id terrans6 = nebula4x::kInvalidId;
  nebula4x::Id pirates6 = nebula4x::kInvalidId;
  for (const auto& [fid, f] : sim6.state().factions) {
    if (f.name == "Terran Union") terrans6 = fid;
    if (f.name == "Pirate Raiders") pirates6 = fid;
  }
  N4X_ASSERT(terrans6 != nebula4x::kInvalidId);
  N4X_ASSERT(pirates6 != nebula4x::kInvalidId);

  nebula4x::Id sol6 = nebula4x::kInvalidId;
  nebula4x::Id cen6 = nebula4x::kInvalidId;
  for (const auto& [sid, sys] : sim6.state().systems) {
    if (sys.name == "Sol") sol6 = sid;
    if (sys.name == "Alpha Centauri") cen6 = sid;
  }
  N4X_ASSERT(sol6 != nebula4x::kInvalidId);
  N4X_ASSERT(cen6 != nebula4x::kInvalidId);

  // Starting discovery should include the system where each faction has assets.
  N4X_ASSERT(sim6.is_system_discovered_by_faction(terrans6, sol6));
  N4X_ASSERT(!sim6.is_system_discovered_by_faction(terrans6, cen6));
  N4X_ASSERT(sim6.is_system_discovered_by_faction(pirates6, cen6));

  // Find a Terran ship in Sol.
  nebula4x::Id terran_ship6 = nebula4x::kInvalidId;
  for (const auto& [sid, sh] : sim6.state().ships) {
    if (sh.faction_id == terrans6 && sh.system_id == sol6) {
      terran_ship6 = sid;
      break;
    }
  }
  N4X_ASSERT(terran_ship6 != nebula4x::kInvalidId);

  // Find the Sol-side jump point.
  nebula4x::Id sol_jump6 = nebula4x::kInvalidId;
  for (const auto& [jid, jp] : sim6.state().jump_points) {
    if (jp.system_id == sol6) {
      sol_jump6 = jid;
      break;
    }
  }
  N4X_ASSERT(sol_jump6 != nebula4x::kInvalidId);

  // Put the ship exactly at the jump point, issue travel, and advance one day.
  // This checks the "transit even if already at jump point" behavior.
  sim6.state().ships[terran_ship6].position_mkm = sim6.state().jump_points[sol_jump6].position_mkm;
  N4X_ASSERT(sim6.issue_travel_via_jump(terran_ship6, sol_jump6));
  sim6.advance_days(1);

  N4X_ASSERT(sim6.state().ships[terran_ship6].system_id == cen6);
  N4X_ASSERT(sim6.is_system_discovered_by_faction(terrans6, cen6));

  return 0;
}
