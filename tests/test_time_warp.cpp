#include <algorithm>
#include <iostream>
#include <string>

#include "nebula4x/core/simulation.h"

#define N4X_ASSERT(expr)                                                                            \
  do {                                                                                              \
    if (!(expr)) {                                                                                  \
      std::cerr << "ASSERT failed: " #expr " (" << __FILE__ << ":" << __LINE__ << ")\n";  \
      return 1;                                                                                     \
    }                                                                                               \
  } while (0)

namespace {

nebula4x::Id min_faction_id(const nebula4x::GameState& st) {
  nebula4x::Id best = nebula4x::kInvalidId;
  for (const auto& [id, _] : st.factions) {
    if (id == nebula4x::kInvalidId) continue;
    if (best == nebula4x::kInvalidId || id < best) best = id;
  }
  return best;
}

} // namespace

int test_time_warp() {
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

  // Test tech that will complete immediately.
  nebula4x::TechDef test_tech;
  test_tech.id = "zz_test_warp_tech";
  test_tech.name = "Warp Tech";
  test_tech.cost = 10.0;
  content.techs[test_tech.id] = test_tech;

  nebula4x::SimConfig cfg;
  cfg.enable_combat = false;
  nebula4x::Simulation sim(std::move(content), cfg);

  auto& st = sim.state();
  const nebula4x::Id fid = min_faction_id(st);
  N4X_ASSERT(fid != nebula4x::kInvalidId);

  // Force the sim close to a day boundary so research ticks on the next hour.
  st.hour_of_day = 23;

  auto* fac = nebula4x::find_ptr(st.factions, fid);
  N4X_ASSERT(fac);
  fac->active_research_id = test_tech.id;
  fac->active_research_progress = test_tech.cost; // remaining == 0 -> immediate completion

  // --- Case 1: stop on Info+Research should hit within 1 hour ---
  {
    nebula4x::EventStopCondition stop;
    stop.stop_on_info = true;
    stop.stop_on_warn = false;
    stop.stop_on_error = false;
    stop.filter_category = true;
    stop.category = nebula4x::EventCategory::Research;
    stop.faction_id = fid;
    stop.message_contains = "Research complete";

    const auto res = sim.advance_until_event_hours(2, stop, /*step_hours=*/1);
    N4X_ASSERT(res.hit);
    N4X_ASSERT(res.hours_advanced == 1);
    N4X_ASSERT(res.event.category == nebula4x::EventCategory::Research);
    N4X_ASSERT(res.event.level == nebula4x::EventLevel::Info);
  }

  // --- Case 2: stop on Warn+Research should not hit (event is Info) ---
  {
    // Reset sim state (fresh run) so we don't rely on already-completed tech.
    nebula4x::ContentDB content2;
    for (const char* id : {"automated_mine", "construction_factory", "shipyard", "research_lab", "sensor_station"}) {
      nebula4x::InstallationDef def;
      def.id = id;
      def.name = id;
      content2.installations[id] = def;
    }
    content2.designs["freighter_alpha"] = make_min_design("freighter_alpha", 10.0);
    content2.designs["surveyor_beta"] = make_min_design("surveyor_beta", 10.0);
    content2.designs["escort_gamma"] = make_min_design("escort_gamma", 10.0);
    content2.designs["pirate_raider"] = make_min_design("pirate_raider", 10.0);
    for (const char* id : {"chemistry_1", "nuclear_1", "propulsion_1"}) {
      nebula4x::TechDef t;
      t.id = id;
      t.name = id;
      t.cost = 1e9;
      content2.techs[id] = t;
    }
    content2.techs[test_tech.id] = test_tech;
    nebula4x::Simulation sim2(std::move(content2), cfg);
    auto& st2 = sim2.state();
    const nebula4x::Id fid2 = min_faction_id(st2);
    N4X_ASSERT(fid2 != nebula4x::kInvalidId);
    st2.hour_of_day = 23;
    auto* fac2 = nebula4x::find_ptr(st2.factions, fid2);
    N4X_ASSERT(fac2);
    fac2->active_research_id = test_tech.id;
    fac2->active_research_progress = test_tech.cost;

    nebula4x::EventStopCondition stop;
    stop.stop_on_info = false;
    stop.stop_on_warn = true;
    stop.stop_on_error = false;
    stop.filter_category = true;
    stop.category = nebula4x::EventCategory::Research;
    stop.faction_id = fid2;

    const auto res = sim2.advance_until_event_hours(2, stop, /*step_hours=*/1);
    N4X_ASSERT(!res.hit);
    N4X_ASSERT(res.hours_advanced == 2);
  }

  return 0;
}
