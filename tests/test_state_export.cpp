#include <cmath>
#include <iostream>
#include <string>

#include "nebula4x/core/game_state.h"
#include "nebula4x/core/orders.h"
#include "nebula4x/util/json.h"
#include "nebula4x/util/state_export.h"

#define N4X_ASSERT(expr)                                                                            \
  do {                                                                                              \
    if (!(expr)) {                                                                                  \
      std::cerr << "ASSERT failed: " #expr " (" << __FILE__ << ":" << __LINE__ << ")\n";   \
      return 1;                                                                                     \
    }                                                                                               \
  } while (0)

int test_state_export() {
  nebula4x::ContentDB content;

  // Minimal design.
  {
    nebula4x::ShipDesign d;
    d.id = "scout";
    d.name = "Scout";
    d.mass_tons = 200.0;
    d.cargo_tons = 50.0;
    d.sensor_range_mkm = 12.0;
    d.max_hp = 20.0;
    d.weapon_damage = 0.0;
    d.weapon_range_mkm = 0.0;
    content.designs[d.id] = d;
  }

  // Installations.
  {
    nebula4x::InstallationDef mine;
    mine.id = "mine";
    mine.name = "Mine";
    mine.produces_per_day = {{"Duranium", 1.5}};
    mine.construction_points_per_day = 0.0;
    mine.build_rate_tons_per_day = 0.0;
    content.installations[mine.id] = mine;

    nebula4x::InstallationDef yard;
    yard.id = "shipyard";
    yard.name = "Shipyard";
    yard.produces_per_day = {};
    yard.construction_points_per_day = 0.0;
    yard.build_rate_tons_per_day = 100.0;
    content.installations[yard.id] = yard;
  }

  // Build a tiny state.
  nebula4x::GameState st;

  const nebula4x::Id fac_id = 1;
  {
    nebula4x::Faction f;
    f.id = fac_id;
    f.name = "Terrans";
    st.factions[f.id] = f;
  }

  const nebula4x::Id sys_id = 10;
  {
    nebula4x::StarSystem sys;
    sys.id = sys_id;
    sys.name = "Sol";
    st.systems[sys.id] = sys;
  }

  const nebula4x::Id body_id = 100;
  {
    nebula4x::Body b;
    b.id = body_id;
    b.name = "Earth";
    b.system_id = sys_id;
    st.bodies[b.id] = b;
  }

  const nebula4x::Id colony_id = 7;
  {
    nebula4x::Colony c;
    c.id = colony_id;
    c.name = "Earth";
    c.faction_id = fac_id;
    c.body_id = body_id;
    c.population_millions = 8500.0;
    c.minerals = {{"Duranium", 123.0}};
    c.installations = {{"mine", 2}, {"shipyard", 1}};

    nebula4x::BuildOrder bo;
    bo.design_id = "scout";
    bo.tons_remaining = 25.0;
    c.shipyard_queue.push_back(bo);

    nebula4x::InstallationBuildOrder io;
    io.installation_id = "mine";
    io.quantity_remaining = 3;
    io.minerals_paid = true;
    io.cp_remaining = 5.0;
    c.construction_queue.push_back(io);

    st.colonies[c.id] = c;
  }

  const nebula4x::Id ship_id = 42;
  {
    nebula4x::Ship sh;
    sh.id = ship_id;
    sh.name = "SC-1";
    sh.faction_id = fac_id;
    sh.system_id = sys_id;
    sh.design_id = "scout";
    sh.position_mkm = {1.0, 2.0};
    sh.speed_km_s = 120.0;
    sh.hp = 10.0;
    sh.cargo = {{"Duranium", 5.0}};
    st.ships[sh.id] = sh;

    nebula4x::ShipOrders so;
    so.queue.push_back(nebula4x::MoveToPoint{nebula4x::Vec2{3.0, 4.0}});
    st.ship_orders[sh.id] = so;
  }

  // Fleet
  {
    nebula4x::Fleet fl;
    fl.id = 99;
    fl.name = "Alpha Fleet";
    fl.faction_id = fac_id;
    fl.leader_ship_id = ship_id;
    fl.ship_ids = {ship_id};
    st.fleets[fl.id] = fl;
  }

  // --- Ships JSON ---
  {
    const std::string text = nebula4x::ships_to_json(st, &content);
    N4X_ASSERT(!text.empty());
    N4X_ASSERT(text.back() == '\n');

    const auto v = nebula4x::json::parse(text);
    const auto* arr = v.as_array();
    N4X_ASSERT(arr);
    N4X_ASSERT(arr->size() == 1);

    const auto* obj = (*arr)[0].as_object();
    N4X_ASSERT(obj);

    N4X_ASSERT(obj->at("id").int_value() == 42);
    N4X_ASSERT(obj->at("design").string_value() == "Scout");
    N4X_ASSERT(std::fabs(obj->at("cargo_used_tons").number_value() - 5.0) < 1e-9);

    const auto* q = obj->at("order_queue").as_array();
    N4X_ASSERT(q);
    N4X_ASSERT(q->size() == 1);
    N4X_ASSERT((*q)[0].string_value().find("MoveToPoint") != std::string::npos);
  }

  // --- Colonies JSON ---
  {
    const std::string text = nebula4x::colonies_to_json(st, &content);
    N4X_ASSERT(!text.empty());
    N4X_ASSERT(text.back() == '\n');

    const auto v = nebula4x::json::parse(text);
    const auto* arr = v.as_array();
    N4X_ASSERT(arr);
    N4X_ASSERT(arr->size() == 1);

    const auto* obj = (*arr)[0].as_object();
    N4X_ASSERT(obj);
    N4X_ASSERT(obj->at("name").string_value() == "Earth");
    N4X_ASSERT(obj->at("system").string_value() == "Sol");
    N4X_ASSERT(obj->at("body").string_value() == "Earth");

    N4X_ASSERT(std::fabs(obj->at("shipyard_capacity_tons_per_day").number_value() - 100.0) < 1e-9);

    const auto* prod = obj->at("mineral_production_per_day").as_object();
    N4X_ASSERT(prod);
    N4X_ASSERT(std::fabs(prod->at("Duranium").number_value() - 3.0) < 1e-9);
  }

  // --- Fleets JSON ---
  {
    const std::string text = nebula4x::fleets_to_json(st);
    N4X_ASSERT(!text.empty());
    N4X_ASSERT(text.back() == '\n');

    const auto v = nebula4x::json::parse(text);
    const auto* arr = v.as_array();
    N4X_ASSERT(arr);
    N4X_ASSERT(arr->size() == 1);

    const auto* obj = (*arr)[0].as_object();
    N4X_ASSERT(obj);
    N4X_ASSERT(obj->at("name").string_value() == "Alpha Fleet");
    N4X_ASSERT(obj->at("leader_ship_name").string_value() == "SC-1");

    const auto* ships = obj->at("ships").as_array();
    N4X_ASSERT(ships);
    N4X_ASSERT(ships->size() == 1);
    const auto* sh = (*ships)[0].as_object();
    N4X_ASSERT(sh);
    N4X_ASSERT(sh->at("id").int_value() == 42);
    N4X_ASSERT(sh->at("system").string_value() == "Sol");
  }

  return 0;
}
