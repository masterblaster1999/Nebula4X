#include <iostream>
#include <string>
#include <variant>

#include "nebula4x/core/serialization.h"
#include "nebula4x/core/simulation.h"

#define N4X_ASSERT(expr)                                                                            \
  do {                                                                                              \
    if (!(expr)) {                                                                                  \
      std::cerr << "ASSERT failed: " #expr " (" << __FILE__ << ":" << __LINE__ << ")\n"; \
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

} // namespace

int test_order_repeat() {
  nebula4x::ContentDB content;

  auto add_min_design = [&](const std::string& id) {
    nebula4x::ShipDesign d;
    d.id = id;
    d.name = id;
    d.speed_km_s = 0.0;
    d.max_hp = 10.0;
    content.designs[id] = d;
  };

  // Ensure default scenario ships have designs.
  add_min_design("freighter_alpha");
  add_min_design("surveyor_beta");
  add_min_design("escort_gamma");
  add_min_design("pirate_raider");

  nebula4x::Simulation sim(std::move(content), nebula4x::SimConfig{});

  const auto ship_id = find_ship_id(sim.state(), "Freighter Alpha");
  N4X_ASSERT(ship_id != nebula4x::kInvalidId);

  // Queue a simple sequence and enable repeat.
  N4X_ASSERT(sim.clear_orders(ship_id));
  N4X_ASSERT(sim.issue_wait_days(ship_id, 1));
  N4X_ASSERT(sim.issue_wait_days(ship_id, 1));

  N4X_ASSERT(sim.enable_order_repeat(ship_id));
  {
    const auto& so = sim.state().ship_orders.at(ship_id);
    N4X_ASSERT(so.repeat);
    N4X_ASSERT(so.repeat_count_remaining == -1);
    N4X_ASSERT(so.queue.size() == 2);
    N4X_ASSERT(so.repeat_template.size() == 2);
    N4X_ASSERT(!nebula4x::ship_orders_is_idle_for_automation(so));
  }

  // After 2 days, both waits should be consumed and the queue should be empty,
  // but repeat should still be enabled.
  sim.advance_days(2);
  {
    const auto& so = sim.state().ship_orders.at(ship_id);
    N4X_ASSERT(so.queue.empty());
    N4X_ASSERT(so.repeat);
    N4X_ASSERT(so.repeat_count_remaining == -1);
    N4X_ASSERT(so.repeat_template.size() == 2);
  }

  // On the next day, the queue should be refilled from the template and the
  // first order should execute, leaving one wait.
  sim.advance_days(1);
  {
    const auto& q = sim.state().ship_orders.at(ship_id).queue;
    N4X_ASSERT(q.size() == 1);
    N4X_ASSERT(std::holds_alternative<nebula4x::WaitDays>(q.front()));
    N4X_ASSERT(std::get<nebula4x::WaitDays>(q.front()).days_remaining == 1);
  }

  // Clear orders should also disable repeat and clear the template.
  N4X_ASSERT(sim.clear_orders(ship_id));
  {
    const auto& so = sim.state().ship_orders.at(ship_id);
    N4X_ASSERT(so.queue.empty());
    N4X_ASSERT(!so.repeat);
    N4X_ASSERT(so.repeat_count_remaining == 0);
    N4X_ASSERT(so.repeat_template.empty());
  }

  // Finite repeat: allow only a single template refill.
  N4X_ASSERT(sim.issue_wait_days(ship_id, 1));
  N4X_ASSERT(sim.issue_wait_days(ship_id, 1));
  N4X_ASSERT(sim.enable_order_repeat(ship_id, 1));
  {
    const auto& so = sim.state().ship_orders.at(ship_id);
    N4X_ASSERT(so.repeat);
    N4X_ASSERT(so.repeat_count_remaining == 1);
    N4X_ASSERT(so.queue.size() == 2);
    N4X_ASSERT(so.repeat_template.size() == 2);
  }

  // After two days the queue empties, but repeat remains enabled.
  sim.advance_days(2);
  {
    const auto& so = sim.state().ship_orders.at(ship_id);
    N4X_ASSERT(so.queue.empty());
    N4X_ASSERT(so.repeat);
    N4X_ASSERT(so.repeat_count_remaining == 1);
  }

  // Next day: template refills once (count becomes 0) and first order executes.
  sim.advance_days(1);
  {
    const auto& so = sim.state().ship_orders.at(ship_id);
    N4X_ASSERT(so.repeat);
    N4X_ASSERT(so.repeat_count_remaining == 0);
    N4X_ASSERT(so.queue.size() == 1);
  }

  // Finish the last order: queue empty; repeat still on but will stop next tick.
  sim.advance_days(1);
  {
    const auto& so = sim.state().ship_orders.at(ship_id);
    N4X_ASSERT(so.queue.empty());
    N4X_ASSERT(so.repeat);
    N4X_ASSERT(so.repeat_count_remaining == 0);
    N4X_ASSERT(nebula4x::ship_orders_is_idle_for_automation(so));
  }

  // Next day: repeat auto-stops (template preserved for manual restart).
  sim.advance_days(1);
  {
    const auto& so = sim.state().ship_orders.at(ship_id);
    N4X_ASSERT(so.queue.empty());
    N4X_ASSERT(!so.repeat);
    N4X_ASSERT(so.repeat_count_remaining == 0);
    N4X_ASSERT(so.repeat_template.size() == 2);
  }

  // Restart using the saved template.
  N4X_ASSERT(sim.enable_order_repeat_from_template(ship_id));
  {
    const auto& so = sim.state().ship_orders.at(ship_id);
    N4X_ASSERT(so.repeat);
    N4X_ASSERT(so.repeat_count_remaining == -1);
    N4X_ASSERT(so.queue.size() == 2);
    N4X_ASSERT(so.repeat_template.size() == 2);
  }

  // Clear again for remaining tests.
  N4X_ASSERT(sim.clear_orders(ship_id));

  // A day later, the queue should remain empty (no refill).
  sim.advance_days(1);
  N4X_ASSERT(sim.state().ship_orders.at(ship_id).queue.empty());

  // Serialization round-trip should preserve repeat state and template.
  N4X_ASSERT(sim.issue_wait_days(ship_id, 3));
  N4X_ASSERT(sim.enable_order_repeat(ship_id));

  const std::string json_text = nebula4x::serialize_game_to_json(sim.state());
  const auto loaded = nebula4x::deserialize_game_from_json(json_text);

  N4X_ASSERT(loaded.ship_orders.count(ship_id) > 0);
  {
    const auto& so = loaded.ship_orders.at(ship_id);
    N4X_ASSERT(so.repeat);
    N4X_ASSERT(so.repeat_count_remaining == -1);
    N4X_ASSERT(so.queue.size() == 1);
    N4X_ASSERT(so.repeat_template.size() == 1);
    N4X_ASSERT(std::holds_alternative<nebula4x::WaitDays>(so.queue[0]));
    N4X_ASSERT(std::holds_alternative<nebula4x::WaitDays>(so.repeat_template[0]));
    N4X_ASSERT(std::get<nebula4x::WaitDays>(so.repeat_template[0]).days_remaining == 3);
  }

  return 0;
}
