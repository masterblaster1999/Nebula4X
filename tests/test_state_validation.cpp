#include <iostream>
#include <string>

#include "nebula4x/core/orders.h"
#include "nebula4x/core/scenario.h"
#include "nebula4x/core/state_validation.h"
#include "nebula4x/core/tech.h"

#define N4X_ASSERT(expr)                                                                            \
  do {                                                                                              \
    if (!(expr)) {                                                                                  \
      std::cerr << "ASSERT failed: " #expr " (" << __FILE__ << ":" << __LINE__ << ")\n";            \
      return 1;                                                                                     \
    }                                                                                               \
  } while (0)

int test_state_validation() {
  // Load default content (used to validate design/tech references).
  auto content = nebula4x::load_content_db_from_file("data/blueprints/starting_blueprints.json");
  content.techs = nebula4x::load_tech_db_from_file("data/tech/tech_tree.json");

  // A freshly generated Sol scenario should be internally consistent.
  {
    const auto st = nebula4x::make_sol_scenario();
    const auto errors = nebula4x::validate_game_state(st, &content);
    if (!errors.empty()) {
      std::cerr << "State validation failed for Sol scenario:\n";
      for (const auto& e : errors) std::cerr << "  - " << e << "\n";
      return 1;
    }
  }

  // Sanity: broken references should be detected.
  {
    auto st = nebula4x::make_sol_scenario();
    N4X_ASSERT(!st.ships.empty());

    const auto ship_it = st.ships.begin();
    st.ships[ship_it->first].system_id = static_cast<nebula4x::Id>(999999);

    const auto errors = nebula4x::validate_game_state(st, &content);

    bool has_bad_system_ref = false;
    for (const auto& e : errors) {
      if (e.find("Ship") != std::string::npos && e.find("unknown system_id") != std::string::npos) {
        has_bad_system_ref = true;
        break;
      }
    }
    N4X_ASSERT(has_bad_system_ref);
  }

  // Sanity: next_id should be monotonic.
  {
    auto st = nebula4x::make_sol_scenario();
    st.next_id = 1; // almost certainly <= max existing id

    const auto errors = nebula4x::validate_game_state(st, &content);
    bool has_next_id = false;
    for (const auto& e : errors) {
      if (e.find("next_id is not monotonic") != std::string::npos) {
        has_next_id = true;
        break;
      }
    }
    N4X_ASSERT(has_next_id);
  }

  // Sanity: ship_orders entries for missing ships should be reported.
  {
    auto st = nebula4x::make_sol_scenario();
    const auto bogus_ship_id = static_cast<nebula4x::Id>(999999);
    st.ship_orders[bogus_ship_id] = nebula4x::ShipOrders{};

    const auto errors = nebula4x::validate_game_state(st, &content);
    bool has_missing_ship_orders = false;
    for (const auto& e : errors) {
      if (e.find("ship_orders contains entry for missing ship id") != std::string::npos) {
        has_missing_ship_orders = true;
        break;
      }
    }
    N4X_ASSERT(has_missing_ship_orders);
  }

  // Sanity: invalid order references should be reported.
  {
    auto st = nebula4x::make_sol_scenario();
    N4X_ASSERT(!st.ships.empty());
    const auto ship_id = st.ships.begin()->first;

    // Inject an order referencing a missing body.
    st.ship_orders[ship_id].queue.push_back(nebula4x::MoveToBody{static_cast<nebula4x::Id>(999999)});

    const auto errors = nebula4x::validate_game_state(st, &content);
    bool has_missing_body_order = false;
    for (const auto& e : errors) {
      if (e.find("MoveToBody references missing body_id") != std::string::npos) {
        has_missing_body_order = true;
        break;
      }
    }
    N4X_ASSERT(has_missing_body_order);
  }

  // Sanity: newer order types should be validated too (and should not be treated as "unknown").
  {
    auto st = nebula4x::make_sol_scenario();
    N4X_ASSERT(st.ships.size() >= 2);
    N4X_ASSERT(!st.colonies.empty());
    N4X_ASSERT(!st.bodies.empty());

    auto it = st.ships.begin();
    const auto ship_a = it->first;
    ++it;
    const auto ship_b = it->first;

    const auto colony_id = st.colonies.begin()->first;
    const auto body_id = st.bodies.begin()->first;

    st.ship_orders[ship_a].queue.clear();
    st.ship_orders[ship_a].queue.push_back(nebula4x::OrbitBody{body_id, 1});
    st.ship_orders[ship_a].queue.push_back(
        nebula4x::TransferCargoToShip{ship_b, "Duranium", 10.0});
    st.ship_orders[ship_a].queue.push_back(nebula4x::ScrapShip{colony_id});

    const auto errors = nebula4x::validate_game_state(st, &content);
    if (!errors.empty()) {
      std::cerr << "Unexpected errors for valid Orbit/Transfer/Scrap orders:\n";
      for (const auto& e : errors) std::cerr << "  - " << e << "\n";
      return 1;
    }
  }

  // Sanity: invalid ids inside newer order types should be reported.
  {
    auto st = nebula4x::make_sol_scenario();
    N4X_ASSERT(st.ships.size() >= 1);
    const auto ship_id = st.ships.begin()->first;

    st.ship_orders[ship_id].queue.push_back(nebula4x::OrbitBody{static_cast<nebula4x::Id>(999999), 1});
    st.ship_orders[ship_id].queue.push_back(nebula4x::OrbitBody{st.bodies.begin()->first, -2});
    st.ship_orders[ship_id].queue.push_back(
        nebula4x::TransferCargoToShip{static_cast<nebula4x::Id>(999999), "Duranium", 1.0});
    st.ship_orders[ship_id].queue.push_back(nebula4x::ScrapShip{static_cast<nebula4x::Id>(999999)});

    const auto errors = nebula4x::validate_game_state(st, &content);
    bool has_orbit_missing_body = false;
    bool has_orbit_bad_duration = false;
    bool has_transfer_missing_ship = false;
    bool has_scrap_missing_colony = false;
    for (const auto& e : errors) {
      if (e.find("OrbitBody references missing body_id") != std::string::npos) has_orbit_missing_body = true;
      if (e.find("OrbitBody has invalid duration_days") != std::string::npos) has_orbit_bad_duration = true;
      if (e.find("TransferCargoToShip references missing target_ship_id") != std::string::npos)
        has_transfer_missing_ship = true;
      if (e.find("ScrapShip references missing colony_id") != std::string::npos) has_scrap_missing_colony = true;
    }
    N4X_ASSERT(has_orbit_missing_body);
    N4X_ASSERT(has_orbit_bad_duration);
    N4X_ASSERT(has_transfer_missing_ship);
    N4X_ASSERT(has_scrap_missing_colony);
  }

  return 0;
}
