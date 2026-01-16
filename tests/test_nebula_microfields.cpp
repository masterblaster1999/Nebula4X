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

nebula4x::Id find_system_id(const nebula4x::GameState& st, const std::string& name) {
  for (const auto& [sid, sys] : st.systems) {
    if (sys.name == name) return sid;
  }
  return nebula4x::kInvalidId;
}

} // namespace

int test_nebula_microfields() {
  using namespace nebula4x;

  // Microfields enabled: local density should vary with position, but remain deterministic.
  {
    ContentDB content;
    SimConfig cfg;
    cfg.enable_nebula_microfields = true;
    cfg.nebula_microfield_strength = 0.35;
    cfg.nebula_microfield_filament_mix = 0.70;
    Simulation sim(std::move(content), cfg);

    const Id sol_id = find_system_id(sim.state(), "Sol");
    N4X_ASSERT(sol_id != kInvalidId);

    // Force a strongly nebular system so variation is visible.
    sim.state().systems[sol_id].nebula_density = 0.70;

    const Vec2 p0{0.0, 0.0};
    const Vec2 p1{250.0, -180.0};
    const Vec2 p2{1500.0, 900.0};

    const double d0 = sim.system_nebula_density_at(sol_id, p0);
    const double d1 = sim.system_nebula_density_at(sol_id, p1);
    const double d2 = sim.system_nebula_density_at(sol_id, p2);

    N4X_ASSERT(d0 >= 0.0 && d0 <= 1.0);
    N4X_ASSERT(d1 >= 0.0 && d1 <= 1.0);
    N4X_ASSERT(d2 >= 0.0 && d2 <= 1.0);

    // Deterministic (same sim, same inputs).
    N4X_ASSERT(std::abs(sim.system_nebula_density_at(sol_id, p0) - d0) < 1e-12);
    N4X_ASSERT(std::abs(sim.system_nebula_density_at(sol_id, p1) - d1) < 1e-12);
    N4X_ASSERT(std::abs(sim.system_nebula_density_at(sol_id, p2) - d2) < 1e-12);

    // Position dependence (almost surely).
    const bool varies = (std::abs(d0 - d1) > 1e-4) || (std::abs(d0 - d2) > 1e-4) ||
                        (std::abs(d1 - d2) > 1e-4);
    N4X_ASSERT(varies);

    const double env0 = sim.system_sensor_environment_multiplier_at(sol_id, p0);
    const double env1 = sim.system_sensor_environment_multiplier_at(sol_id, p1);
    const double env2 = sim.system_sensor_environment_multiplier_at(sol_id, p2);

    N4X_ASSERT(env0 > 0.0 && env0 <= 1.0);
    N4X_ASSERT(env1 > 0.0 && env1 <= 1.0);
    N4X_ASSERT(env2 > 0.0 && env2 <= 1.0);

    const bool env_varies = (std::abs(env0 - env1) > 1e-6) || (std::abs(env0 - env2) > 1e-6);
    N4X_ASSERT(env_varies);
  }

  // Microfields disabled: local density should equal the system baseline everywhere.
  {
    ContentDB content;
    SimConfig cfg;
    cfg.enable_nebula_microfields = false;
    Simulation sim(std::move(content), cfg);

    const Id sol_id = find_system_id(sim.state(), "Sol");
    N4X_ASSERT(sol_id != kInvalidId);

    sim.state().systems[sol_id].nebula_density = 0.70;

    const Vec2 p0{0.0, 0.0};
    const Vec2 p1{250.0, -180.0};
    const Vec2 p2{1500.0, 900.0};

    const double d0 = sim.system_nebula_density_at(sol_id, p0);
    const double d1 = sim.system_nebula_density_at(sol_id, p1);
    const double d2 = sim.system_nebula_density_at(sol_id, p2);

    N4X_ASSERT(std::abs(d0 - 0.70) < 1e-9);
    N4X_ASSERT(std::abs(d1 - 0.70) < 1e-9);
    N4X_ASSERT(std::abs(d2 - 0.70) < 1e-9);
  }

  return 0;
}
