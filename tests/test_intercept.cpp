#include "nebula4x/core/intercept.h"

#include "tests/test.h"

#include <cmath>
#include <iostream>

namespace {

#define N4X_ASSERT(cond)                                                                          \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      std::cerr << "ASSERT FAILED: " << #cond << " at " << __FILE__ << ":" << __LINE__ << "\n"; \
      return 1;                                                                                    \
    }                                                                                              \
  } while (0)

inline bool near(double a, double b, double eps = 1e-6) {
  return std::abs(a - b) <= eps;
}

} // namespace

int test_intercept() {
  using nebula4x::Vec2;
  using nebula4x::compute_intercept_aim;

  // 1) Stationary target, zero range.
  {
    const Vec2 P{0.0, 0.0};
    const Vec2 T{10.0, 0.0};
    const Vec2 V{0.0, 0.0};

    const auto aim = compute_intercept_aim(P, /*speed=*/10.0, T, V, /*range=*/0.0, /*max_lead=*/100.0);
    N4X_ASSERT(aim.has_solution);
    N4X_ASSERT(near(aim.solution_time_days, 1.0, 1e-6));
    N4X_ASSERT(near(aim.aim_position_mkm.x, 10.0, 1e-6));
    N4X_ASSERT(near(aim.aim_position_mkm.y, 0.0, 1e-6));
  }

  // 2) Stationary target, non-zero desired range.
  {
    const Vec2 P{0.0, 0.0};
    const Vec2 T{10.0, 0.0};
    const Vec2 V{0.0, 0.0};

    const auto aim = compute_intercept_aim(P, /*speed=*/10.0, T, V, /*range=*/2.0, /*max_lead=*/100.0);
    N4X_ASSERT(aim.has_solution);
    N4X_ASSERT(near(aim.solution_time_days, 0.8, 1e-6));
    N4X_ASSERT(near(aim.aim_position_mkm.x, 10.0, 1e-6));
    N4X_ASSERT(near(aim.aim_position_mkm.y, 0.0, 1e-6));
  }

  // 3) Target moving away faster than pursuer, no intercept solution.
  {
    const Vec2 P{0.0, 0.0};
    const Vec2 T{10.0, 0.0};
    const Vec2 V{20.0, 0.0};

    const auto aim = compute_intercept_aim(P, /*speed=*/10.0, T, V, /*range=*/0.0, /*max_lead=*/100.0);
    N4X_ASSERT(!aim.has_solution);
    N4X_ASSERT(near(aim.aim_position_mkm.x, 10.0, 1e-6));
    N4X_ASSERT(near(aim.aim_position_mkm.y, 0.0, 1e-6));
  }

  // 4) Linear-ish case (a ~= 0).
  {
    const Vec2 P{0.0, 0.0};
    const Vec2 T{0.0, 10.0};
    const Vec2 V{10.0, 0.0};

    const auto aim = compute_intercept_aim(P, /*speed=*/10.0, T, V, /*range=*/0.0, /*max_lead=*/100.0);
    // There is a valid intercept when the target crosses our path.
    N4X_ASSERT(aim.has_solution);
    N4X_ASSERT(aim.solution_time_days >= 0.0);
  }

  return 0;
}
