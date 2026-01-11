#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>

#include "nebula4x/core/simulation.h"

#define N4X_ASSERT(expr)                                                                            \
  do {                                                                                              \
    if (!(expr)) {                                                                                  \
      std::cerr << "ASSERT failed: " #expr " (" << __FILE__ << ":" << __LINE__ << ")\n";    \
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

int test_turn_ticks() {
  nebula4x::ContentDB content;

  // Minimal installations referenced by the default scenario.
  for (const char* id : {"automated_mine", "construction_factory", "shipyard", "research_lab", "sensor_station"}) {
    nebula4x::InstallationDef def;
    def.id = id;
    def.name = id;
    content.installations[id] = def;
  }

  auto make_min_design = [](const std::string& id, double speed_km_s) {
    nebula4x::ShipDesign d;
    d.id = id;
    d.name = id;
    d.max_hp = 50.0;
    d.speed_km_s = speed_km_s;
    d.sensor_range_mkm = 0.0;
    return d;
  };

  // Scenario ship designs.
  content.designs["freighter_alpha"] = make_min_design("freighter_alpha", 10.0);
  content.designs["surveyor_beta"] = make_min_design("surveyor_beta", 10.0);
  content.designs["escort_gamma"] = make_min_design("escort_gamma", 10.0);
  content.designs["pirate_raider"] = make_min_design("pirate_raider", 10.0);

  // Minimal techs referenced by the scenario.
  for (const char* id : {"chemistry_1", "nuclear_1", "propulsion_1"}) {
    nebula4x::TechDef t;
    t.id = id;
    t.name = id;
    t.cost = 1e9;
    content.techs[id] = t;
  }

  nebula4x::SimConfig cfg;
  cfg.enable_combat = false; // keep this test deterministic + cheap
  nebula4x::Simulation sim(std::move(content), cfg);

  auto& st = sim.state();
  const int d0 = st.date.days_since_epoch();
  N4X_ASSERT(st.hour_of_day == 0);

  // --- timekeeping ---
  sim.advance_hours(1);
  N4X_ASSERT(st.date.days_since_epoch() == d0);
  N4X_ASSERT(st.hour_of_day == 1);

  sim.advance_hours(23);
  N4X_ASSERT(st.date.days_since_epoch() == d0 + 1);
  N4X_ASSERT(st.hour_of_day == 0);

  // Mid-day +1d should preserve hour-of-day and advance the calendar by one day.
  sim.advance_hours(12);
  N4X_ASSERT(st.hour_of_day == 12);
  const int d1 = st.date.days_since_epoch();
  sim.advance_days(1);
  N4X_ASSERT(st.date.days_since_epoch() == d1 + 1);
  N4X_ASSERT(st.hour_of_day == 12);

  // --- movement scaling ---
  const auto freighter_id = find_ship_id(st, "Freighter Alpha");
  N4X_ASSERT(freighter_id != nebula4x::kInvalidId);
  auto* sh = nebula4x::find_ptr(st.ships, freighter_id);
  N4X_ASSERT(sh);

  // Force a simple MoveToPoint order.
  sim.clear_orders(freighter_id);
  const nebula4x::Vec2 start = sh->position_mkm;
  const nebula4x::Vec2 goal = start + nebula4x::Vec2{100.0, 0.0};
  N4X_ASSERT(sim.issue_move_to_point(freighter_id, goal));

  // One hour of movement should equal (speed_km_s * seconds_per_day / 1e6) / 24.
  const double per_day_mkm = sh->speed_km_s * sim.cfg().seconds_per_day / 1e6;
  const double expected = per_day_mkm / 24.0;

  sim.advance_hours(1);
  const nebula4x::Vec2 after = sh->position_mkm;
  const double moved = (after - start).length();
  N4X_ASSERT(std::isfinite(moved));
  N4X_ASSERT(std::abs(moved - expected) < 1e-3);

  // --- movement scaling (nebula storms) ---
  // Inject a deterministic storm and ensure movement is slowed.
  {
    auto* sys = nebula4x::find_ptr(st.systems, sh->system_id);
    N4X_ASSERT(sys);
    const std::int64_t now = st.date.days_since_epoch();
    sys->storm_peak_intensity = 1.0;
    sys->storm_start_day = now - 1;
    sys->storm_end_day = now + 1;

    sim.clear_orders(freighter_id);
    const nebula4x::Vec2 start2 = sh->position_mkm;
    const nebula4x::Vec2 goal2 = start2 + nebula4x::Vec2{100.0, 0.0};
    N4X_ASSERT(sim.issue_move_to_point(freighter_id, goal2));

    sim.advance_hours(1);
    const nebula4x::Vec2 after2 = sh->position_mkm;
    const double moved2 = (after2 - start2).length();
    const double env2 = sim.system_movement_speed_multiplier(sh->system_id);
    const double expected2 = expected * env2;
    N4X_ASSERT(std::isfinite(moved2));
    N4X_ASSERT(env2 < 0.999);
    N4X_ASSERT(std::abs(moved2 - expected2) < 1e-3);
  }


  // --- WaitDays should not consume a full day on a sub-day tick ---
  sim.clear_orders(freighter_id);
  N4X_ASSERT(sim.issue_wait_days(freighter_id, 1));
  sim.advance_hours(12);
  {
    auto it = st.ship_orders.find(freighter_id);
    N4X_ASSERT(it != st.ship_orders.end());
    N4X_ASSERT(!it->second.queue.empty());
  }
  sim.advance_hours(12);
  {
    auto it = st.ship_orders.find(freighter_id);
    if (it != st.ship_orders.end()) {
      N4X_ASSERT(it->second.queue.empty());
    }
  }

  return 0;
}
