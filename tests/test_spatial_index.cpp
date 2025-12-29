#include <cassert>

#include "nebula4x/util/spatial_index.h"

namespace nebula4x {

int test_spatial_index() {
  // Insertion order should not affect deterministic query results.
  {
    SpatialIndex2D idx(10.0);
    idx.add(3, Vec2{20.0, 0.0});
    idx.add(1, Vec2{0.0, 0.0});
    idx.add(2, Vec2{9.9, 0.0});

    const auto r = idx.query_radius(Vec2{0.0, 0.0}, 10.0);
    assert((r == std::vector<Id>{1, 2}));
  }

  // Negative coordinates should be handled correctly (floor cell mapping).
  {
    SpatialIndex2D idx(10.0);
    idx.add(1, Vec2{-0.1, -0.1});
    idx.add(2, Vec2{-9.9, -9.9});
    idx.add(3, Vec2{-20.0, 0.0});

    const auto r = idx.query_radius(Vec2{0.0, 0.0}, 15.0);
    // ids 1 and 2 are within ~0.141 and ~14.0, id 3 is 20 away.
    assert((r == std::vector<Id>{1, 2}));
  }

  // Epsilon should widen the inclusion radius when requested.
  {
    SpatialIndex2D idx(10.0);
    idx.add(1, Vec2{10.0, 0.0});
    // With radius 10, ship at exactly 10 should be included.
    const auto r0 = idx.query_radius(Vec2{0.0, 0.0}, 10.0, 0.0);
    assert((r0 == std::vector<Id>{1}));
    // With a slightly smaller radius, epsilon brings it back.
    const auto r1 = idx.query_radius(Vec2{0.0, 0.0}, 9.999, 0.01);
    assert((r1 == std::vector<Id>{1}));
  }

  return 0;
}

} // namespace nebula4x
