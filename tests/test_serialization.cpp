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

  // 3) CSV escaping helper should behave as expected.
  N4X_ASSERT(nebula4x::csv_escape("hello") == "hello");
  N4X_ASSERT(nebula4x::csv_escape("a,b") == "\"a,b\"");
  N4X_ASSERT(nebula4x::csv_escape("a\"b") == "\"a\"\"b\"");
  N4X_ASSERT(nebula4x::csv_escape("a\nb") == "\"a\nb\"");

  return 0;
}
