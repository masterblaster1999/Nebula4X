#include <iostream>
#include <string>

#include "nebula4x/core/entities.h"
#include "nebula4x/core/procgen_surface.h"

#define N4X_ASSERT(expr)                                                                            \
  do {                                                                                              \
    if (!(expr)) {                                                                                  \
      std::cerr << "ASSERT failed: " #expr " (" << __FILE__ << ":" << __LINE__ << ")\n";           \
      return 1;                                                                                     \
    }                                                                                               \
  } while (0)

int test_procgen_surface() {
  using namespace nebula4x;

  Body earthish;
  earthish.id = 1;
  earthish.system_id = 2;
  earthish.type = BodyType::Planet;
  earthish.name = "Test Terra";
  earthish.surface_temp_k = 288.0;
  earthish.atmosphere_atm = 1.0;
  earthish.orbit_radius_mkm = 149.6;

  const auto f1 = procgen_surface::flavor(earthish, 26, 12);
  const auto f2 = procgen_surface::flavor(earthish, 26, 12);

  // Deterministic.
  N4X_ASSERT(f1.biome == f2.biome);
  N4X_ASSERT(f1.stamp == f2.stamp);
  N4X_ASSERT(f1.legend == f2.legend);
  N4X_ASSERT(f1.quirks.size() == f2.quirks.size());

  // Basic structure: border + content.
  N4X_ASSERT(!f1.stamp.empty());
  N4X_ASSERT(f1.stamp.find('+') != std::string::npos);
  N4X_ASSERT(f1.stamp.find('|') != std::string::npos);

  // Biome sanity for a temperate, ~1 atm world.
  N4X_ASSERT(f1.biome.find("Temperate") != std::string::npos);

  // Nearby body id should produce a different stamp (almost surely).
  Body other = earthish;
  other.id = 2;
  const auto f3 = procgen_surface::flavor(other, 26, 12);
  N4X_ASSERT(f3.stamp != f1.stamp);

  // Gas giant stamp should be classified and contain band characters.
  Body jove;
  jove.id = 100;
  jove.system_id = 2;
  jove.type = BodyType::GasGiant;
  jove.name = "Test Jove";
  jove.surface_temp_k = 130.0;
  jove.atmosphere_atm = 0.0;
  const auto gj = procgen_surface::flavor(jove, 26, 12);
  N4X_ASSERT(gj.biome == "Gas Giant");
  N4X_ASSERT(gj.stamp.find('=') != std::string::npos || gj.stamp.find('-') != std::string::npos);

  return 0;
}
