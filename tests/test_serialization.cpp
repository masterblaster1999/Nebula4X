#include <algorithm>
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

  // 1) Round-trip serialization should preserve basic counts.
  const std::string json_text = nebula4x::serialize_game_to_json(sim.state());
  const auto loaded = nebula4x::deserialize_game_from_json(json_text);

  N4X_ASSERT(loaded.systems.size() == sim.state().systems.size());
  N4X_ASSERT(loaded.bodies.size() == sim.state().bodies.size());
  N4X_ASSERT(loaded.ships.size() == sim.state().ships.size());
  N4X_ASSERT(loaded.colonies.size() == sim.state().colonies.size());

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

  return 0;
}
