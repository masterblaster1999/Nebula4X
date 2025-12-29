#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>
#include <utility>

#include "nebula4x/core/serialization.h"
#include "nebula4x/core/simulation.h"
#include "nebula4x/util/json.h"
#include "nebula4x/util/strings.h"

#define N4X_ASSERT(expr) \
  do { \
    if (!(expr)) { \
      std::cerr << "ASSERT failed: " #expr " (" << __FILE__ << ":" << __LINE__ << ")\n"; \
      return 1; \
    } \
  } while (0)

int test_serialization() {
  // Minimal content DB for installations / designs.
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

  // Toggle some non-default fields to validate schema round-trip.
  nebula4x::Id probe_ship = nebula4x::kInvalidId;
  if (!sim.state().ships.empty()) {
    probe_ship = sim.state().ships.begin()->first;
    sim.state().ships[probe_ship].auto_explore = true;
    sim.state().ships[probe_ship].auto_freight = true;
  }
  for (auto& [_, f] : sim.state().factions) {
    if (f.name == "Pirate Raiders") {
      f.control = nebula4x::FactionControl::AI_Pirate;
    }
  }


  // Configure manual mineral reserves on one colony (new in save schema v25).
  nebula4x::Id reserve_colony_id = nebula4x::kInvalidId;
  if (!sim.state().colonies.empty()) {
    reserve_colony_id = sim.state().colonies.begin()->first;
    sim.state().colonies[reserve_colony_id].mineral_reserves["Duranium"] = 123.0;
    sim.state().colonies[reserve_colony_id].mineral_reserves["Fuel"] = 45.0;
  }

  // 1) Round-trip serialization should preserve basic counts.
  const std::string json_text = nebula4x::serialize_game_to_json(sim.state());
  const auto loaded = nebula4x::deserialize_game_from_json(json_text);

  N4X_ASSERT(loaded.systems.size() == sim.state().systems.size());
  N4X_ASSERT(loaded.bodies.size() == sim.state().bodies.size());
  N4X_ASSERT(loaded.ships.size() == sim.state().ships.size());
  N4X_ASSERT(loaded.colonies.size() == sim.state().colonies.size());
  N4X_ASSERT(loaded.factions.size() == sim.state().factions.size());


  // Mineral reserves should survive round-trip.
  if (reserve_colony_id != nebula4x::kInvalidId) {
    const auto itc = loaded.colonies.find(reserve_colony_id);
    N4X_ASSERT(itc != loaded.colonies.end());
    const auto itd = itc->second.mineral_reserves.find("Duranium");
    N4X_ASSERT(itd != itc->second.mineral_reserves.end());
    N4X_ASSERT(std::abs(itd->second - 123.0) < 1e-6);
    const auto itf = itc->second.mineral_reserves.find("Fuel");
    N4X_ASSERT(itf != itc->second.mineral_reserves.end());
    N4X_ASSERT(std::abs(itf->second - 45.0) < 1e-6);
  }

  if (probe_ship != nebula4x::kInvalidId) {
    const auto it = loaded.ships.find(probe_ship);
    N4X_ASSERT(it != loaded.ships.end());
    N4X_ASSERT(it->second.auto_explore == true);
    N4X_ASSERT(it->second.auto_freight == true);
  }

  bool found_pirates = false;
  for (const auto& [_, f] : loaded.factions) {
    if (f.name == "Pirate Raiders") {
      found_pirates = true;
      N4X_ASSERT(f.control == nebula4x::FactionControl::AI_Pirate);
    }
  }
  N4X_ASSERT(found_pirates);

  // 2) Backwards compatibility: "shipyard_queue" should be optional.
  nebula4x::json::Value root = nebula4x::json::parse(json_text);
  auto* root_obj = root.as_object();
  N4X_ASSERT(root_obj != nullptr);

  auto it_cols = root_obj->find("colonies");
  N4X_ASSERT(it_cols != root_obj->end());

  auto* cols_arr = it_cols->second.as_array();
  N4X_ASSERT(cols_arr != nullptr);

  for (auto& col_v : *cols_arr) {
    auto* col_obj = col_v.as_object();
    N4X_ASSERT(col_obj != nullptr);
    col_obj->erase("shipyard_queue");
  }

  const std::string json_no_queue = nebula4x::json::stringify(root, 2);
  const auto loaded_no_queue = nebula4x::deserialize_game_from_json(json_no_queue);

  for (const auto& [_, c] : loaded_no_queue.colonies) {
    N4X_ASSERT(c.shipyard_queue.empty());
  }

  // 3) Extra backwards compatibility: early prototypes / hand-edited saves may be missing
  // bookkeeping fields like save_version/next_id/selected_system. Deserialization should still work
  // and repair sane defaults.
  {
    nebula4x::json::Value root2 = nebula4x::json::parse(json_text);
    auto* obj2 = root2.as_object();
    N4X_ASSERT(obj2 != nullptr);

    obj2->erase("save_version");
    obj2->erase("next_id");
    obj2->erase("selected_system");
    obj2->erase("next_event_seq");

    const std::string json_missing_meta = nebula4x::json::stringify(root2, 2);
    const auto loaded_missing_meta = nebula4x::deserialize_game_from_json(json_missing_meta);

    N4X_ASSERT(loaded_missing_meta.save_version == nebula4x::GameState{}.save_version);
    N4X_ASSERT(loaded_missing_meta.selected_system == nebula4x::kInvalidId);

    nebula4x::Id max_id = 0;
    auto bump = [&](nebula4x::Id id) { max_id = std::max(max_id, id); };
    for (const auto& [id, _] : loaded_missing_meta.systems) bump(id);
    for (const auto& [id, _] : loaded_missing_meta.bodies) bump(id);
    for (const auto& [id, _] : loaded_missing_meta.jump_points) bump(id);
    for (const auto& [id, _] : loaded_missing_meta.ships) bump(id);
    for (const auto& [id, _] : loaded_missing_meta.colonies) bump(id);
    for (const auto& [id, _] : loaded_missing_meta.factions) bump(id);

    N4X_ASSERT(loaded_missing_meta.next_id > max_id);
    N4X_ASSERT(loaded_missing_meta.next_id != 0);
  }

  // 4) CSV escaping helper should behave as expected.
  N4X_ASSERT(nebula4x::csv_escape("hello") == "hello");
  N4X_ASSERT(nebula4x::csv_escape("a,b") == "\"a,b\"");
  N4X_ASSERT(nebula4x::csv_escape("a\"b") == "\"a\"\"b\"");
  N4X_ASSERT(nebula4x::csv_escape("a\nb") == "\"a\nb\"");

  // 5) Forward compatibility: unknown/invalid ship order entries should not prevent a save
  // from loading. (Older strict behavior would throw and abort the whole load.)
  {
    nebula4x::json::Value root3 = nebula4x::json::parse(json_text);
    auto* obj3 = root3.as_object();
    N4X_ASSERT(obj3 != nullptr);

    // Pick an existing ship id.
    N4X_ASSERT(!sim.state().ships.empty());
    const nebula4x::Id ship_id = sim.state().ships.begin()->first;
    N4X_ASSERT(ship_id != nebula4x::kInvalidId);

    nebula4x::json::Object ship_orders_obj;
    ship_orders_obj["ship_id"] = static_cast<double>(ship_id);

    nebula4x::json::Object future_order;
    future_order["type"] = std::string("some_future_order_type");
    future_order["foo"] = 1.0;

    nebula4x::json::Array q;
    q.push_back(future_order);
    ship_orders_obj["queue"] = q;

    nebula4x::json::Array ship_orders_arr;
    ship_orders_arr.push_back(ship_orders_obj);
    (*obj3)["ship_orders"] = ship_orders_arr;

    const std::string json_bad_order = nebula4x::json::stringify(root3, 2);

    nebula4x::GameState loaded_bad;
    try {
      loaded_bad = nebula4x::deserialize_game_from_json(json_bad_order);
    } catch (...) {
      N4X_ASSERT(false);
    }

    const auto itso = loaded_bad.ship_orders.find(ship_id);
    N4X_ASSERT(itso != loaded_bad.ship_orders.end());
    // Unknown order should be dropped.
    N4X_ASSERT(itso->second.queue.empty());
  }

  // 6) Order templates should round-trip in saves.
  {
    N4X_ASSERT(!sim.state().ships.empty());
    const nebula4x::Id ship_id = sim.state().ships.begin()->first;

    // Ensure a non-empty queue.
    sim.clear_orders(ship_id);
    N4X_ASSERT(sim.issue_wait_days(ship_id, 3));
    N4X_ASSERT(sim.issue_move_to_point(ship_id, nebula4x::Vec2{1.0, 2.0}));

    const auto itso = sim.state().ship_orders.find(ship_id);
    N4X_ASSERT(itso != sim.state().ship_orders.end());
    N4X_ASSERT(!itso->second.queue.empty());

    std::string err;
    N4X_ASSERT(sim.save_order_template("TestTemplate", itso->second.queue, true, &err));

    const std::string json_with_templates = nebula4x::serialize_game_to_json(sim.state());
    const auto loaded_with_templates = nebula4x::deserialize_game_from_json(json_with_templates);

    const auto ittpl = loaded_with_templates.order_templates.find("TestTemplate");
    N4X_ASSERT(ittpl != loaded_with_templates.order_templates.end());
    N4X_ASSERT(ittpl->second.size() == itso->second.queue.size());
  }

  return 0;
}
