#include <iostream>
#include <string>

#include "nebula4x/core/simulation.h"

#define N4X_ASSERT(expr)                                                                            \
  do {                                                                                              \
    if (!(expr)) {                                                                                  \
      std::cerr << "ASSERT failed: " #expr " (" << __FILE__ << ":" << __LINE__ << ")\n";      \
      return 1;                                                                                     \
    }                                                                                               \
  } while (0)

namespace {

nebula4x::GameState make_min_jump_state(nebula4x::Id& fac_id, nebula4x::Id& sys_a, nebula4x::Id& sys_b,
                                       nebula4x::Id& jp_a, nebula4x::Id& jp_b, nebula4x::Id& ship_id) {
  using namespace nebula4x;

  GameState s;
  s.save_version = GameState{}.save_version;
  s.date = Date::from_ymd(2200, 1, 1);
  s.hour_of_day = 0;
  s.next_id = 1;

  fac_id = allocate_id(s);
  {
    Faction f;
    f.id = fac_id;
    f.name = "Faction";
    f.control = FactionControl::Player;
    s.factions[fac_id] = f;
  }

  sys_a = allocate_id(s);
  {
    StarSystem sys;
    sys.id = sys_a;
    sys.name = "SysA";
    sys.galaxy_pos = {0.0, 0.0};
    s.systems[sys_a] = sys;
  }

  sys_b = allocate_id(s);
  {
    StarSystem sys;
    sys.id = sys_b;
    sys.name = "SysB";
    sys.galaxy_pos = {10.0, 0.0};
    s.systems[sys_b] = sys;
  }

  // Mark initial discovery so UI/fog checks in future changes don't surprise tests.
  s.factions[fac_id].discovered_systems = {sys_a};

  jp_a = allocate_id(s);
  jp_b = allocate_id(s);

  {
    JumpPoint a;
    a.id = jp_a;
    a.name = "JP-A";
    a.system_id = sys_a;
    a.position_mkm = {0.0, 0.0};
    a.linked_jump_id = jp_b;
    s.jump_points[jp_a] = a;
    s.systems[sys_a].jump_points.push_back(jp_a);
  }

  {
    JumpPoint b;
    b.id = jp_b;
    b.name = "JP-B";
    b.system_id = sys_b;
    b.position_mkm = {0.0, 0.0};
    b.linked_jump_id = jp_a;
    s.jump_points[jp_b] = b;
    s.systems[sys_b].jump_points.push_back(jp_b);
  }

  ship_id = allocate_id(s);
  {
    Ship sh;
    sh.id = ship_id;
    sh.name = "Scout";
    sh.faction_id = fac_id;
    sh.system_id = sys_a;
    sh.position_mkm = {0.0, 0.0};
    sh.design_id = "scout";
    sh.hp = 50.0;
    sh.shields = 20.0;
    sh.engines_integrity = 1.0;
    sh.sensors_integrity = 1.0;
    sh.weapons_integrity = 1.0;
    sh.shields_integrity = 1.0;

    s.ships[ship_id] = sh;
    s.systems[sys_a].ships.push_back(ship_id);
  }

  return s;
}

}  // namespace

int test_jump_transit_hazards() {
  using namespace nebula4x;

  // Minimal content: a single design.
  ContentDB content;
  {
    ShipDesign d;
    d.id = "scout";
    d.name = "Scout";
    d.speed_km_s = 100.0;
    d.sensor_range_mkm = 10.0;
    d.max_hp = 50.0;
    d.max_shields = 20.0;
    content.designs[d.id] = d;
  }

  // --- Case 1: hazards disabled => no damage.
  {
    SimConfig cfg;
    cfg.enable_jump_point_phenomena = true;
    cfg.jump_phenomena_transit_hazard_strength = 0.0;
    cfg.jump_phenomena_misjump_strength = 0.0;
    cfg.jump_phenomena_subsystem_glitch_strength = 0.0;

    Simulation sim(content, cfg);

    Id fac_id, sys_a, sys_b, jp_a, jp_b, ship_id;
    GameState s = make_min_jump_state(fac_id, sys_a, sys_b, jp_a, jp_b, ship_id);
    sim.load_game(std::move(s));

    N4X_ASSERT(sim.clear_orders(ship_id));
    N4X_ASSERT(sim.issue_travel_via_jump(ship_id, jp_a));

    sim.advance_hours(1);

    const auto* sh = find_ptr(sim.state().ships, ship_id);
    N4X_ASSERT(sh);
    N4X_ASSERT(sh->system_id == sys_b);
    N4X_ASSERT(std::abs(sh->hp - 50.0) < 1e-9);
    N4X_ASSERT(std::abs(sh->shields - 20.0) < 1e-9);

    N4X_ASSERT(sim.is_jump_point_surveyed_by_faction(fac_id, jp_a));
    N4X_ASSERT(sim.is_jump_point_surveyed_by_faction(fac_id, jp_b));
  }

  // --- Case 2: hazard strength cranked => guaranteed (clamped) incident and damage.
  {
    SimConfig cfg;
    cfg.enable_jump_point_phenomena = true;
    cfg.jump_phenomena_transit_hazard_strength = 1000.0;
    cfg.jump_phenomena_misjump_strength = 0.0;
    cfg.jump_phenomena_subsystem_glitch_strength = 0.0;

    Simulation sim(content, cfg);

    Id fac_id, sys_a, sys_b, jp_a, jp_b, ship_id;
    GameState s = make_min_jump_state(fac_id, sys_a, sys_b, jp_a, jp_b, ship_id);
    sim.load_game(std::move(s));

    N4X_ASSERT(sim.clear_orders(ship_id));
    N4X_ASSERT(sim.issue_travel_via_jump(ship_id, jp_a));

    sim.advance_hours(1);

    const auto* sh = find_ptr(sim.state().ships, ship_id);
    N4X_ASSERT(sh);
    N4X_ASSERT(sh->system_id == sys_b);

    // Non-lethal, but should reduce shields/hp.
    N4X_ASSERT(sh->hp >= 1.0 - 1e-9);
    N4X_ASSERT(sh->shields < 20.0 - 1e-9 || sh->hp < 50.0 - 1e-9);
  }

  return 0;
}
