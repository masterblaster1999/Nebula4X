#include <cmath>
#include <iostream>
#include <string>

#include "nebula4x/core/simulation.h"

#define N4X_ASSERT(expr)                                                                            \
  do {                                                                                              \
    if (!(expr)) {                                                                                  \
      std::cerr << "ASSERT failed: " #expr " (" << __FILE__ << ":" << __LINE__ << ")\n";           \
      return 1;                                                                                     \
    }                                                                                               \
  } while (0)

namespace {

bool approx(double a, double b, double eps = 1e-9) { return std::fabs(a - b) <= eps; }

} // namespace

int test_jump_route_env_cost() {
  using namespace nebula4x;

  ContentDB content;

  // Configuration: exaggerate nebula drag + microfield variation so LOS cost differs
  // from the system-average multiplier in a deterministic way.
  SimConfig cfg;
  cfg.seconds_per_day = 86400.0;

  cfg.enable_nebula_drag = true;
  cfg.nebula_drag_speed_penalty_at_max_density = 0.95;

  cfg.enable_nebula_microfields = true;
  cfg.nebula_microfield_strength = 1.0;
  cfg.nebula_microfield_sharpness = 4.0;
  cfg.nebula_microfield_filament_mix = 0.85;
  cfg.nebula_microfield_scale_mkm = 250.0;
  cfg.nebula_microfield_warp_scale_mkm = 500.0;

  Simulation sim(content, cfg);
  auto& st = sim.state();

  st.systems.clear();
  st.bodies.clear();
  st.colonies.clear();
  st.ships.clear();
  st.jump_points.clear();
  st.ship_orders.clear();
  st.fleets.clear();
  st.factions.clear();

  // Two simple systems with a single jump link.
  StarSystem sys1;
  sys1.id = 1;
  sys1.name = "NebulaSys";
  sys1.nebula_density = 0.60;  // base (microfields modulate around this)

  StarSystem sys2;
  sys2.id = 2;
  sys2.name = "ClearSys";
  sys2.nebula_density = 0.0;

  st.systems[sys1.id] = sys1;
  st.systems[sys2.id] = sys2;

  JumpPoint jp_a;
  jp_a.id = 10;
  jp_a.name = "JP A";
  jp_a.system_id = sys1.id;
  jp_a.linked_jump_id = 11;

  JumpPoint jp_b;
  jp_b.id = 11;
  jp_b.name = "JP B";
  jp_b.system_id = sys2.id;
  jp_b.linked_jump_id = 10;

  // Deterministically search for a segment where LOS-integrated cost differs
  // noticeably from the system-average speed multiplier approximation.
  Vec2 start{0.0, 0.0};
  Vec2 end{5000.0, 0.0};
  double baseline_eff = 0.0;
  double los_eff = 0.0;
  bool found = false;

  const double env_global = std::clamp(sim.system_movement_speed_multiplier(sys1.id), 0.05, 1.0);

  // Try a small set of candidate rays (fast + deterministic).
  for (int i = 0; i < 64 && !found; ++i) {
    // Sweep along x with a slight y offset pattern to avoid accidental symmetry.
    const double sx = -8000.0 + 250.0 * static_cast<double>(i);
    const double sy = (i % 2 == 0) ? 0.0 : 333.0;
    const double ex = sx + 9000.0;
    const double ey = sy;

    const Vec2 a{sx, sy};
    const Vec2 b{ex, ey};

    const double leg = (b - a).length();
    const double base = leg / env_global;
    const double los = sim.system_movement_environment_cost_los(sys1.id, a, b, 0ULL);

    // Look for a case where microfields measurably matter.
    const double rel = std::fabs(los - base) / std::max(1e-9, base);
    if (rel >= 0.05) {  // >=5% difference
      start = a;
      end = b;
      baseline_eff = base;
      los_eff = los;
      found = true;
    }
  }

  N4X_ASSERT(found);

  jp_a.position_mkm = end;
  jp_b.position_mkm = {0.0, 0.0};  // irrelevant for cost (jump transit is instantaneous)

  st.jump_points[jp_a.id] = jp_a;
  st.jump_points[jp_b.id] = jp_b;

  st.systems[sys1.id].jump_points.push_back(jp_a.id);
  st.systems[sys2.id].jump_points.push_back(jp_b.id);

  Ship sh;
  sh.id = 100;
  sh.name = "Routed";
  sh.faction_id = 1;
  sh.system_id = sys1.id;
  sh.position_mkm = start;
  sh.speed_km_s = 1000.0;
  st.ships[sh.id] = sh;

  // Sanity: ensure LOS cost is measurably different from the old approximation.
  N4X_ASSERT(std::fabs(los_eff - baseline_eff) > 1e-3);

  // Route to sys2: the entire (effective) distance should be the in-system leg to JP A.
  const auto plan_opt = sim.plan_jump_route_for_ship(sh.id, sys2.id, /*restrict_to_discovered=*/false,
                                                     /*include_queued_jumps=*/false);
  N4X_ASSERT(plan_opt.has_value());
  const JumpRoutePlan& plan = *plan_opt;

  N4X_ASSERT(plan.systems.size() == 2);
  N4X_ASSERT(plan.jump_ids.size() == 1);
  N4X_ASSERT(plan.jump_ids[0] == jp_a.id);

  const double leg_mkm = (end - start).length();
  N4X_ASSERT(approx(plan.distance_mkm, leg_mkm, 1e-9));

  // The key assertion: jump routing uses LOS-integrated environment cost (microfields matter).
  N4X_ASSERT(approx(plan.effective_distance_mkm, los_eff, 1e-6));

  return 0;
}
