#include <iostream>
#include <limits>
#include <variant>

#include "nebula4x/core/orders.h"
#include "nebula4x/core/simulation.h"

namespace nebula4x {

#define N4X_ASSERT(expr) \
  do { \
    if (!(expr)) { \
      std::cerr << "Assertion failed at " << __FILE__ << ":" << __LINE__ << " - " << #expr << "\n"; \
      return 1; \
    } \
  } while (0)

// Auto-refuel should not select colonies that are unreachable with current fuel.
int test_auto_refuel() {
  ContentDB content;
  content.ship_designs["burner"] = ShipDesign{
      .id = "burner",
      .name = "Burner",
      .role = ShipRole::Civilian,
      .tonnage = 1000.0,
      .fuel_capacity_tons = 200.0,
      .fuel_use_per_mkm = 1.0,
  };

  GameState st;
  st.save_version = 39;
  st.next_id = 2000;
  st.date = Date{.days_since_epoch = 1, .hour_of_day = 0};
  st.selected_system = 1;

  // Player faction knows both systems and both sides of the jump.
  st.factions[1] = Faction{
      .id = 1,
      .name = "Player",
      .type = FactionType::Player,
      .discovered_systems = {1, 2},
      .surveyed_jump_points = {100, 101},
  };

  // Two systems connected by a surveyed jump.
  st.star_systems[1] = StarSystem{
      .id = 1,
      .name = "Sol",
      .galaxy_position_mly = Vec2{0.0, 0.0},
      .jump_points = {100},
  };
  st.star_systems[2] = StarSystem{
      .id = 2,
      .name = "Alpha",
      .galaxy_position_mly = Vec2{10.0, 0.0},
      .jump_points = {101},
  };

  st.jump_points[100] = JumpPoint{
      .id = 100,
      .system_id = 1,
      .linked_jump_point_id = 101,
      .position_mkm = Vec2{20.0, 0.0},
      .radius_mkm = 1.0,
      .name = "JP-Sol",
  };
  st.jump_points[101] = JumpPoint{
      .id = 101,
      .system_id = 2,
      .linked_jump_point_id = 100,
      .position_mkm = Vec2{0.0, 0.0},
      .radius_mkm = 1.0,
      .name = "JP-Alpha",
  };

  // Bodies with colonies.
  st.bodies[10] = Body{
      .id = 10,
      .name = "Homeworld",
      .system_id = 1,
      .position_mkm = Vec2{0.0, 0.0},
      .radius_m = 6.0e6,
  };

  st.bodies[20] = Body{
      .id = 20,
      .name = "Fuel Depot World",
      .system_id = 2,
      // Far from the entry jump to ensure it is unreachable with low fuel.
      .position_mkm = Vec2{100.0, 0.0},
      .radius_m = 6.0e6,
  };

  st.colonies[500] = Colony{
      .id = 500,
      .name = "Home Colony",
      .faction_id = 1,
      .body_id = 10,
  };

  st.colonies[501] = Colony{
      .id = 501,
      .name = "Fuel Depot",
      .faction_id = 1,
      .body_id = 20,
  };
  st.colonies[501].minerals["Fuel"] = 1000.0;

  // A ship low on fuel, not docked, with auto-refuel enabled.
  st.ships[200] = Ship{
      .id = 200,
      .name = "Refuel Test Ship",
      .design_id = "burner",
      .faction_id = 1,
      .system_id = 1,
      .position_mkm = Vec2{20.0, 0.0},
      .speed_km_s = 1000.0,
      .fuel_tons = 40.0, // 20% fuel; default threshold is 25%
      .auto_refuel = true,
      .auto_refuel_threshold_fraction = 0.25,
  };

  Simulation sim;
  sim.set_content_db(content);
  sim.load_game(st);

  sim.run_ai_planning();

  const auto& out = sim.state();
  const auto* orders = find_ptr(out.ship_orders, Id{200});
  N4X_ASSERT(orders != nullptr);
  N4X_ASSERT(orders->queue.size() == 1);
  N4X_ASSERT(std::holds_alternative<MoveToBody>(orders->queue[0]));
  const auto& move = std::get<MoveToBody>(orders->queue[0]);
  // The ship should choose the reachable colony in-system, even though it has no fuel.
  N4X_ASSERT(move.body_id == 10);

  return 0;
}

} // namespace nebula4x
