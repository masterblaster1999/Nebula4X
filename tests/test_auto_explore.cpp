#include <iostream>
#include <string>
#include <variant>
#include <algorithm>
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

nebula4x::Id find_jump_between(const nebula4x::GameState& st, nebula4x::Id from_sys, nebula4x::Id to_sys) {
  const auto* sys = nebula4x::find_ptr(st.systems, from_sys);
  if (!sys) return nebula4x::kInvalidId;

  std::vector<nebula4x::Id> jps = sys->jump_points;
  std::sort(jps.begin(), jps.end());

  for (nebula4x::Id jp_id : jps) {
    const auto* jp = nebula4x::find_ptr(st.jump_points, jp_id);
    if (!jp) continue;
    const auto* other = nebula4x::find_ptr(st.jump_points, jp->linked_jump_id);
    if (!other) continue;
    if (other->system_id == to_sys) return jp_id;
  }

  return nebula4x::kInvalidId;
}

void remove_id(std::vector<nebula4x::Id>& v, nebula4x::Id x) {
  v.erase(std::remove(v.begin(), v.end(), x), v.end());
}

} // namespace

int test_auto_explore() {
  nebula4x::ContentDB content;

  auto add_min_design = [&](const std::string& id) {
    nebula4x::ShipDesign d;
    d.id = id;
    d.name = id;
    d.speed_km_s = 100.0;  // non-zero so route planning is valid
    d.max_hp = 10.0;
    content.designs[id] = d;
  };

  // Ensure default scenario ships have designs (keeps stats deterministic).
  add_min_design("freighter_alpha");
  add_min_design("surveyor_beta");
  add_min_design("escort_gamma");
  add_min_design("pirate_raider");

  nebula4x::Simulation sim(std::move(content), nebula4x::SimConfig{});

  const auto scout_id = find_ship_id(sim.state(), "Surveyor Beta");
  N4X_ASSERT(scout_id != nebula4x::kInvalidId);

  const auto sol = find_system_id(sim.state(), "Sol");
  const auto cen = find_system_id(sim.state(), "Alpha Centauri");
  const auto bar = find_system_id(sim.state(), "Barnard's Star");
  N4X_ASSERT(sol != nebula4x::kInvalidId);
  N4X_ASSERT(cen != nebula4x::kInvalidId);
  N4X_ASSERT(bar != nebula4x::kInvalidId);

  const auto jp_sol_to_cen = find_jump_between(sim.state(), sol, cen);
  const auto jp_cen_to_bar = find_jump_between(sim.state(), cen, bar);
  N4X_ASSERT(jp_sol_to_cen != nebula4x::kInvalidId);
  N4X_ASSERT(jp_cen_to_bar != nebula4x::kInvalidId);

  const auto jp_cen_to_sol = sim.state().jump_points.at(jp_sol_to_cen).linked_jump_id;
  N4X_ASSERT(jp_cen_to_sol != nebula4x::kInvalidId);

  const auto jp_bar_to_cen = sim.state().jump_points.at(jp_cen_to_bar).linked_jump_id;
  N4X_ASSERT(jp_bar_to_cen != nebula4x::kInvalidId);

  // Disable auto-explore on other ships to keep the test deterministic.
  for (auto& [sid, sh] : sim.state().ships) {
    sh.auto_explore = (sid == scout_id);
  }

  // Configure faction knowledge:
  // - Discovered: Sol and Centauri only.
  // - Surveyed: the Sol<->Centauri link only.
  //   (Centauri's exit to Barnard is *unsurveyed*.)
  const auto fid = sim.state().ships.at(scout_id).faction_id;
  N4X_ASSERT(fid != nebula4x::kInvalidId);

  auto& fac = sim.state().factions.at(fid);
  fac.discovered_systems = {sol, cen};
  fac.surveyed_jump_points = {jp_sol_to_cen, jp_cen_to_sol};

  // Make sure the Barnard link isn't accidentally in the survey list.
  remove_id(fac.surveyed_jump_points, jp_cen_to_bar);
  remove_id(fac.surveyed_jump_points, jp_bar_to_cen);

  // --- Case 1: ship in Sol should route to frontier system (Centauri) ---
  {
    N4X_ASSERT(sim.clear_orders(scout_id));
    sim.state().ships[scout_id].system_id = sol;

    sim.run_ai_planning();

    const auto& q = sim.state().ship_orders.at(scout_id).queue;
    N4X_ASSERT(q.size() == 1);
    N4X_ASSERT(std::holds_alternative<nebula4x::TravelViaJump>(q[0]));
    const auto jid = std::get<nebula4x::TravelViaJump>(q[0]).jump_point_id;
    N4X_ASSERT(jid == jp_sol_to_cen);
  }

  // --- Case 2: ship in Centauri with an UNSURVEYED exit should issue survey-oriented navigation.
  // Depending on implementation details this can be either:
  // - MoveToPoint to the jump point position, or
  // - SurveyJumpPoint directly (which may include optional transit_when_done behavior).
  {
    N4X_ASSERT(sim.clear_orders(scout_id));
    sim.state().ships[scout_id].system_id = cen;

    sim.run_ai_planning();

    const auto& q = sim.state().ship_orders.at(scout_id).queue;
    N4X_ASSERT(q.size() == 1);
    const auto* jp = nebula4x::find_ptr(sim.state().jump_points, jp_cen_to_bar);
    N4X_ASSERT(jp);

    if (std::holds_alternative<nebula4x::MoveToPoint>(q[0])) {
      const auto target = std::get<nebula4x::MoveToPoint>(q[0]).target_mkm;
      // Exact match when using explicit move-to-jump behavior.
      N4X_ASSERT(target.x == jp->position_mkm.x);
      N4X_ASSERT(target.y == jp->position_mkm.y);
    } else {
      N4X_ASSERT(std::holds_alternative<nebula4x::SurveyJumpPoint>(q[0]));
      const auto ord = std::get<nebula4x::SurveyJumpPoint>(q[0]);
      N4X_ASSERT(ord.jump_point_id == jp_cen_to_bar);
    }
  }

  // --- Case 3: if the exit is SURVEYED and leads to an undiscovered system, auto-explore should jump ---
  {
    fac.surveyed_jump_points.push_back(jp_cen_to_bar);

    N4X_ASSERT(sim.clear_orders(scout_id));
    sim.state().ships[scout_id].system_id = cen;

    sim.run_ai_planning();

    const auto& q = sim.state().ship_orders.at(scout_id).queue;
    N4X_ASSERT(q.size() == 1);
    N4X_ASSERT(std::holds_alternative<nebula4x::TravelViaJump>(q[0]));
    const auto jid = std::get<nebula4x::TravelViaJump>(q[0]).jump_point_id;
    N4X_ASSERT(jid == jp_cen_to_bar);
  }

  return 0;
}
