#include <iostream>
#include <string>
#include <variant>
#include <algorithm>
#include <utility>
#include <vector>

#include "nebula4x/core/simulation.h"

#define N4X_ASSERT(expr)                                                                            \
  do {                                                                                              \
    if (!(expr)) {                                                                                  \
      std::cerr << "ASSERT failed: " #expr " (" << __FILE__ << ":" << __LINE__ << ")\n";           \
      return 1;                                                                                     \
    }                                                                                               \
  } while (0)

namespace {

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

nebula4x::Id find_body_id(const nebula4x::GameState& st, const std::string& name) {
  for (const auto& [bid, b] : st.bodies) {
    if (b.name == name) return bid;
  }
  return nebula4x::kInvalidId;
}

nebula4x::Id find_colony_id(const nebula4x::GameState& st, const std::string& name) {
  for (const auto& [cid, c] : st.colonies) {
    if (c.name == name) return cid;
  }
  return nebula4x::kInvalidId;
}

} // namespace

int test_auto_routing() {
  nebula4x::ContentDB content;

  auto add_min_design = [&](const std::string& id) {
    nebula4x::ShipDesign d;
    d.id = id;
    d.name = id;
    d.speed_km_s = 0.0;
    d.max_hp = 10.0;
    content.designs[id] = d;
  };

  // Ensure default scenario ships have designs (keeps stats deterministic).
  add_min_design("freighter_alpha");
  add_min_design("surveyor_beta");
  add_min_design("escort_gamma");
  add_min_design("pirate_raider");

  nebula4x::Simulation sim(std::move(content), nebula4x::SimConfig{});

  const auto ship_id = find_ship_id(sim.state(), "Freighter Alpha");
  N4X_ASSERT(ship_id != nebula4x::kInvalidId);

  const auto sol_sys = find_system_id(sim.state(), "Sol");
  const auto cen_sys = find_system_id(sim.state(), "Alpha Centauri");
  const auto bar_sys = find_system_id(sim.state(), "Barnard's Star");
  N4X_ASSERT(sol_sys != nebula4x::kInvalidId);
  N4X_ASSERT(cen_sys != nebula4x::kInvalidId);
  N4X_ASSERT(bar_sys != nebula4x::kInvalidId);

  const auto cen_prime = find_body_id(sim.state(), "Centauri Prime");
  const auto barnard_b = find_body_id(sim.state(), "Barnard b");
  N4X_ASSERT(cen_prime != nebula4x::kInvalidId);
  N4X_ASSERT(barnard_b != nebula4x::kInvalidId);

  // --- move-to-body auto-routes across systems ---
  {
    N4X_ASSERT(sim.clear_orders(ship_id));
    N4X_ASSERT(sim.issue_move_to_body(ship_id, cen_prime));

    const auto& q = sim.state().ship_orders.at(ship_id).queue;
    N4X_ASSERT(q.size() == 2);

    N4X_ASSERT(std::holds_alternative<nebula4x::TravelViaJump>(q[0]));
    const auto jid = std::get<nebula4x::TravelViaJump>(q[0]).jump_point_id;
    const auto* jp = nebula4x::find_ptr(sim.state().jump_points, jid);
    N4X_ASSERT(jp);
    N4X_ASSERT(jp->system_id == sol_sys);

    const auto* dest = nebula4x::find_ptr(sim.state().jump_points, jp->linked_jump_id);
    N4X_ASSERT(dest);
    N4X_ASSERT(dest->system_id == cen_sys);

    N4X_ASSERT(std::holds_alternative<nebula4x::MoveToBody>(q[1]));
    N4X_ASSERT(std::get<nebula4x::MoveToBody>(q[1]).body_id == cen_prime);
  }


  // --- attack auto-routes across systems (via last-known contact system) ---
  {
    const auto escort_id = find_ship_id(sim.state(), "Escort Gamma");
    N4X_ASSERT(escort_id != nebula4x::kInvalidId);

    const auto raider_id = find_ship_id(sim.state(), "Raider I");
    N4X_ASSERT(raider_id != nebula4x::kInvalidId);

    const auto terrans = sim.state().ships.at(escort_id).faction_id;

    // Having a contact in a system implies the faction has discovered it.
    {
      auto& ds = sim.state().factions.at(terrans).discovered_systems;
      if (std::find(ds.begin(), ds.end(), cen_sys) == ds.end()) ds.push_back(cen_sys);
    }

    // Seed a contact snapshot for the raider in Centauri so the attack order can be issued even
    // without current detection.
    {
      const auto& raider = sim.state().ships.at(raider_id);
      nebula4x::Contact c;
      c.ship_id = raider_id;
      c.system_id = cen_sys;
      c.last_seen_day = static_cast<int>(sim.state().date.days_since_epoch());
      c.last_seen_position_mkm = raider.position_mkm;
      c.last_seen_name = raider.name;
      c.last_seen_design_id = raider.design_id;
      c.last_seen_faction_id = raider.faction_id;
      sim.state().factions[terrans].ship_contacts[raider_id] = c;
    }

    N4X_ASSERT(sim.clear_orders(escort_id));
    N4X_ASSERT(sim.issue_attack_ship(escort_id, raider_id, true));

    const auto& q = sim.state().ship_orders.at(escort_id).queue;
    N4X_ASSERT(q.size() == 2);
    N4X_ASSERT(std::holds_alternative<nebula4x::TravelViaJump>(q[0]));
    N4X_ASSERT(std::holds_alternative<nebula4x::AttackShip>(q[1]));

    const auto j = std::get<nebula4x::TravelViaJump>(q[0]).jump_point_id;
    const auto* jp = nebula4x::find_ptr(sim.state().jump_points, j);
    N4X_ASSERT(jp);
    N4X_ASSERT(jp->system_id == sol_sys);
    const auto* dest = nebula4x::find_ptr(sim.state().jump_points, jp->linked_jump_id);
    N4X_ASSERT(dest);
    N4X_ASSERT(dest->system_id == cen_sys);
  }

  // --- queued travel routes start from the end-of-queue system ---
  {
    N4X_ASSERT(sim.clear_orders(ship_id));
    N4X_ASSERT(sim.issue_travel_to_system(ship_id, cen_sys));
    N4X_ASSERT(sim.issue_travel_to_system(ship_id, bar_sys));

    const auto& q = sim.state().ship_orders.at(ship_id).queue;
    N4X_ASSERT(q.size() == 2);

    N4X_ASSERT(std::holds_alternative<nebula4x::TravelViaJump>(q[0]));
    N4X_ASSERT(std::holds_alternative<nebula4x::TravelViaJump>(q[1]));

    const auto j1 = std::get<nebula4x::TravelViaJump>(q[0]).jump_point_id;
    const auto* jp1 = nebula4x::find_ptr(sim.state().jump_points, j1);
    N4X_ASSERT(jp1);
    N4X_ASSERT(jp1->system_id == sol_sys);
    const auto* d1 = nebula4x::find_ptr(sim.state().jump_points, jp1->linked_jump_id);
    N4X_ASSERT(d1);
    N4X_ASSERT(d1->system_id == cen_sys);

    const auto j2 = std::get<nebula4x::TravelViaJump>(q[1]).jump_point_id;
    const auto* jp2 = nebula4x::find_ptr(sim.state().jump_points, j2);
    N4X_ASSERT(jp2);
    N4X_ASSERT(jp2->system_id == cen_sys);
    const auto* d2 = nebula4x::find_ptr(sim.state().jump_points, jp2->linked_jump_id);
    N4X_ASSERT(d2);
    N4X_ASSERT(d2->system_id == bar_sys);
  }

  // --- cargo orders can auto-route across systems ---
  {
    const auto earth = find_colony_id(sim.state(), "Earth");
    N4X_ASSERT(earth != nebula4x::kInvalidId);

    // Use the Earth colony's owning faction (avoids hard-coding faction names).
    const auto terrans = sim.state().colonies.at(earth).faction_id;
    N4X_ASSERT(terrans != nebula4x::kInvalidId);
    N4X_ASSERT(nebula4x::find_ptr(sim.state().factions, terrans));

    nebula4x::Colony outpost;
    outpost.id = nebula4x::allocate_id(sim.state());
    outpost.name = "Centauri Outpost";
    outpost.faction_id = terrans;
    outpost.body_id = cen_prime;
    outpost.population_millions = 1.0;
    outpost.minerals["Duranium"] = 0.0;

    sim.state().colonies[outpost.id] = outpost;

    N4X_ASSERT(sim.clear_orders(ship_id));
    N4X_ASSERT(sim.issue_load_mineral(ship_id, earth, "Duranium", 10.0));
    N4X_ASSERT(sim.issue_unload_mineral(ship_id, outpost.id, "Duranium", 10.0));

    const auto& q = sim.state().ship_orders.at(ship_id).queue;
    N4X_ASSERT(q.size() == 3);

    N4X_ASSERT(std::holds_alternative<nebula4x::LoadMineral>(q[0]));
    N4X_ASSERT(std::holds_alternative<nebula4x::TravelViaJump>(q[1]));
    N4X_ASSERT(std::holds_alternative<nebula4x::UnloadMineral>(q[2]));

    const auto jid = std::get<nebula4x::TravelViaJump>(q[1]).jump_point_id;
    const auto* jp = nebula4x::find_ptr(sim.state().jump_points, jid);
    N4X_ASSERT(jp);
    N4X_ASSERT(jp->system_id == sol_sys);
    const auto* dest = nebula4x::find_ptr(sim.state().jump_points, jp->linked_jump_id);
    N4X_ASSERT(dest);
    N4X_ASSERT(dest->system_id == cen_sys);

    N4X_ASSERT(std::get<nebula4x::UnloadMineral>(q[2]).colony_id == outpost.id);
  }

  // --- same-system orders appended after travel will auto-route back ---
  {
    const auto earth = find_colony_id(sim.state(), "Earth");
    N4X_ASSERT(earth != nebula4x::kInvalidId);

    N4X_ASSERT(sim.clear_orders(ship_id));
    N4X_ASSERT(sim.issue_travel_to_system(ship_id, cen_sys));
    N4X_ASSERT(sim.issue_load_mineral(ship_id, earth, "Duranium", 1.0));

    const auto& q = sim.state().ship_orders.at(ship_id).queue;
    N4X_ASSERT(q.size() == 3);

    N4X_ASSERT(std::holds_alternative<nebula4x::TravelViaJump>(q[0]));
    N4X_ASSERT(std::holds_alternative<nebula4x::TravelViaJump>(q[1]));
    N4X_ASSERT(std::holds_alternative<nebula4x::LoadMineral>(q[2]));

    const auto j1 = std::get<nebula4x::TravelViaJump>(q[0]).jump_point_id;
    const auto* jp1 = nebula4x::find_ptr(sim.state().jump_points, j1);
    N4X_ASSERT(jp1);
    N4X_ASSERT(jp1->system_id == sol_sys);
    const auto* d1 = nebula4x::find_ptr(sim.state().jump_points, jp1->linked_jump_id);
    N4X_ASSERT(d1);
    N4X_ASSERT(d1->system_id == cen_sys);

    const auto j2 = std::get<nebula4x::TravelViaJump>(q[1]).jump_point_id;
    const auto* jp2 = nebula4x::find_ptr(sim.state().jump_points, j2);
    N4X_ASSERT(jp2);
    N4X_ASSERT(jp2->system_id == cen_sys);
    const auto* d2 = nebula4x::find_ptr(sim.state().jump_points, jp2->linked_jump_id);
    N4X_ASSERT(d2);
    N4X_ASSERT(d2->system_id == sol_sys);
  }

  // --- smart order template application should inject missing travel ---
  {
    const auto earth = find_colony_id(sim.state(), "Earth");
    N4X_ASSERT(earth != nebula4x::kInvalidId);

    const auto outpost_id = find_colony_id(sim.state(), "Centauri Outpost");
    N4X_ASSERT(outpost_id != nebula4x::kInvalidId);

    std::vector<nebula4x::Order> tpl;
    tpl.push_back(nebula4x::LoadMineral{earth, "Duranium", 1.0});
    tpl.push_back(nebula4x::UnloadMineral{outpost_id, "Duranium", 1.0});

    std::string err;
    N4X_ASSERT(sim.save_order_template("tpl_duranium_run", tpl, /*overwrite=*/true, &err));

    // Create a new ship in Alpha Centauri to ensure the template remains usable from a different starting system.
    auto& s = sim.state();
    const nebula4x::Id new_ship_id = nebula4x::allocate_id(s);

    const auto* base_ship = nebula4x::find_ptr(s.ships, ship_id);
    N4X_ASSERT(base_ship);

    auto new_ship = *base_ship;
    new_ship.id = new_ship_id;
    new_ship.name = "Template Runner";
    new_ship.system_id = cen_sys;
    new_ship.position_mkm = {0.0, 0.0};
    s.ships[new_ship_id] = new_ship;
    s.systems[cen_sys].ships.push_back(new_ship_id);

    err.clear();
    N4X_ASSERT(sim.apply_order_template_to_ship_smart(new_ship_id, "tpl_duranium_run", /*append=*/false,
                                                     /*restrict_to_discovered=*/false, &err));

    const auto& q = s.ship_orders.at(new_ship_id).queue;
    N4X_ASSERT(q.size() == 4);

    N4X_ASSERT(std::holds_alternative<nebula4x::TravelViaJump>(q[0]));
    N4X_ASSERT(std::holds_alternative<nebula4x::LoadMineral>(q[1]));
    N4X_ASSERT(std::holds_alternative<nebula4x::TravelViaJump>(q[2]));
    N4X_ASSERT(std::holds_alternative<nebula4x::UnloadMineral>(q[3]));

    const auto j0 = std::get<nebula4x::TravelViaJump>(q[0]).jump_point_id;
    const auto* jp0 = nebula4x::find_ptr(s.jump_points, j0);
    N4X_ASSERT(jp0);
    N4X_ASSERT(jp0->system_id == cen_sys);

    const auto j2 = std::get<nebula4x::TravelViaJump>(q[2]).jump_point_id;
    const auto* jp2 = nebula4x::find_ptr(s.jump_points, j2);
    N4X_ASSERT(jp2);
    N4X_ASSERT(jp2->system_id == sol_sys);
  }

  return 0;
}
