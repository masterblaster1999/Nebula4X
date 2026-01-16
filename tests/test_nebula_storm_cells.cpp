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

int test_nebula_storm_cells() {
  using namespace nebula4x;

  // Storm cells enabled: local storm intensity should vary with position,
  // but remain deterministic.
  {
    ContentDB content;
    SimConfig cfg;
    cfg.enable_nebula_storms = true;
    cfg.enable_nebula_storm_cells = true;
    cfg.nebula_storm_cell_strength = 0.85;
    cfg.nebula_storm_cell_scale_mkm = 1400.0;
    cfg.nebula_storm_cell_drift_speed_mkm_per_day = 180.0;
    cfg.nebula_storm_cell_sharpness = 1.7;

    Simulation sim(std::move(content), cfg);

    const Id sol_id = find_system_id(sim.state(), "Sol");
    N4X_ASSERT(sol_id != kInvalidId);

    auto& sys = sim.state().systems[sol_id];
    sys.nebula_density = 0.75;

    // Force a storm window and place the clock at mid-storm.
    sys.storm_peak_intensity = 0.75;
    sys.storm_start_day = sim.state().date.days_since_epoch();
    sys.storm_end_day = sys.storm_start_day + 10;

    sim.state().date = Date(sys.storm_start_day + 5);
    sim.state().hour_of_day = 0;

    const double base = sim.system_storm_intensity(sol_id);
    N4X_ASSERT(base > 0.70 && base < 0.80);

    const Vec2 p0{0.0, 0.0};
    const Vec2 p1{250.0, -180.0};
    const Vec2 p2{1500.0, 900.0};

    const double s0 = sim.system_storm_intensity_at(sol_id, p0);
    const double s1 = sim.system_storm_intensity_at(sol_id, p1);
    const double s2 = sim.system_storm_intensity_at(sol_id, p2);

    N4X_ASSERT(s0 >= 0.0 && s0 <= 1.0);
    N4X_ASSERT(s1 >= 0.0 && s1 <= 1.0);
    N4X_ASSERT(s2 >= 0.0 && s2 <= 1.0);

    // Deterministic (same sim, same inputs).
    N4X_ASSERT(std::abs(sim.system_storm_intensity_at(sol_id, p0) - s0) < 1e-12);
    N4X_ASSERT(std::abs(sim.system_storm_intensity_at(sol_id, p1) - s1) < 1e-12);
    N4X_ASSERT(std::abs(sim.system_storm_intensity_at(sol_id, p2) - s2) < 1e-12);

    // Position dependence (almost surely).
    const bool varies = (std::abs(s0 - s1) > 1e-4) || (std::abs(s0 - s2) > 1e-4) ||
                        (std::abs(s1 - s2) > 1e-4);
    N4X_ASSERT(varies);
  }

  // Storm cells disabled: local storm intensity should equal the system baseline everywhere.
  {
    ContentDB content;
    SimConfig cfg;
    cfg.enable_nebula_storms = true;
    cfg.enable_nebula_storm_cells = false;

    Simulation sim(std::move(content), cfg);

    const Id sol_id = find_system_id(sim.state(), "Sol");
    N4X_ASSERT(sol_id != kInvalidId);

    auto& sys = sim.state().systems[sol_id];
    sys.nebula_density = 0.75;

    sys.storm_peak_intensity = 0.65;
    sys.storm_start_day = sim.state().date.days_since_epoch();
    sys.storm_end_day = sys.storm_start_day + 10;

    sim.state().date = Date(sys.storm_start_day + 5);
    sim.state().hour_of_day = 0;

    const double base = sim.system_storm_intensity(sol_id);
    N4X_ASSERT(base > 0.60 && base < 0.70);

    const Vec2 p0{0.0, 0.0};
    const Vec2 p1{250.0, -180.0};
    const Vec2 p2{1500.0, 900.0};

    const double s0 = sim.system_storm_intensity_at(sol_id, p0);
    const double s1 = sim.system_storm_intensity_at(sol_id, p1);
    const double s2 = sim.system_storm_intensity_at(sol_id, p2);

    N4X_ASSERT(std::abs(s0 - base) < 1e-12);
    N4X_ASSERT(std::abs(s1 - base) < 1e-12);
    N4X_ASSERT(std::abs(s2 - base) < 1e-12);
  }

  return 0;
}
