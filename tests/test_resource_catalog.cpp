#include <iostream>
#include <string>
#include <vector>

#include "nebula4x/core/tech.h"

#define N4X_ASSERT(expr)                                                                            \
  do {                                                                                              \
    if (!(expr)) {                                                                                  \
      std::cerr << __FILE__ << ":" << __LINE__ << ": Assertion failed: " << #expr << std::endl; \
      return 1;                                                                                     \
    }                                                                                               \
  } while (0)

int test_resource_catalog() {
  using namespace nebula4x;

  const ContentDB content =
      load_content_db_from_file("data/blueprints/starting_blueprints.json");

  // A small, stable roster of materials (Aurora-style) that we expect to exist in the default data.
  const std::vector<std::string> expected = {
      "Duranium",   "Neutronium", "Corbomite", "Tritanium", "Boronide", "Mercassium",
      "Vendarite",  "Sorium",     "Uridium",   "Corundium", "Gallicite", "Fuel",
  };

  for (const auto& rid : expected) {
    N4X_ASSERT(content.resources.count(rid) == 1);
    N4X_ASSERT(!content.resources.at(rid).name.empty());
    N4X_ASSERT(!content.resources.at(rid).category.empty());
  }

  // Fuel is manufactured, not mineable.
  N4X_ASSERT(content.resources.at("Fuel").mineable == false);

  // Minerals are mineable.
  for (const auto& rid : expected) {
    if (rid == "Fuel") continue;
    N4X_ASSERT(content.resources.at(rid).mineable == true);
  }

  // Default mining installation uses the generic "tons/day" mining model.
  {
    const auto& mine = content.installations.at("automated_mine");
    N4X_ASSERT(mine.mining);
    N4X_ASSERT(mine.mining_tons_per_day > 0.0);
  }

  // Default fuel refinery consumes Sorium.
  {
    const auto& refinery = content.installations.at("fuel_refinery");
    N4X_ASSERT(refinery.consumes_per_day.count("Sorium") == 1);
    N4X_ASSERT(refinery.consumes_per_day.count("Duranium") == 0);
    N4X_ASSERT(refinery.consumes_per_day.count("Neutronium") == 0);
    N4X_ASSERT(refinery.produces_per_day.count("Fuel") == 1);
  }

  // Shipbuilding uses a multi-resource composition.
  {
    const auto& shipyard = content.installations.at("shipyard");
    N4X_ASSERT(shipyard.build_costs_per_ton.count("Duranium") == 1);
    N4X_ASSERT(shipyard.build_costs_per_ton.count("Neutronium") == 1);
    N4X_ASSERT(shipyard.build_costs_per_ton.count("Gallicite") == 1);
    N4X_ASSERT(shipyard.build_costs_per_ton.count("Corundium") == 1);
  }

  return 0;
}
