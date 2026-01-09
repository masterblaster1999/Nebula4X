#include <iostream>
#include <string>

#include "nebula4x/core/content_validation.h"
#include "nebula4x/core/tech.h"

#define N4X_ASSERT(expr)                                                                            \
  do {                                                                                              \
    if (!(expr)) {                                                                                  \
      std::cerr << "ASSERT failed: " #expr " (" << __FILE__ << ":" << __LINE__ << ")\n";            \
      return 1;                                                                                     \
    }                                                                                               \
  } while (0)

int test_missile_components() {
  auto content = nebula4x::load_content_db_from_file("data/blueprints/starting_blueprints.json");
  content.techs = nebula4x::load_tech_db_from_file("data/tech/tech_tree.json");

  // Ensure the default content remains valid with missiles/PD enabled.
  {
    const auto errors = nebula4x::validate_content_db(content);
    if (!errors.empty()) {
      std::cerr << "Content validation failed:\n";
      for (const auto& e : errors) std::cerr << "  - " << e << "\n";
      return 1;
    }
  }

  const auto it_m = content.components.find("missile_rack_mk1");
  N4X_ASSERT(it_m != content.components.end());
  const auto& m = it_m->second;
  N4X_ASSERT(m.missile_damage > 0.0);
  N4X_ASSERT(m.missile_range_mkm > 0.0);
  N4X_ASSERT(m.missile_speed_mkm_per_day > 0.0);

  const auto it_pd = content.components.find("pd_laser_mk1");
  N4X_ASSERT(it_pd != content.components.end());
  const auto& pd = it_pd->second;
  N4X_ASSERT(pd.point_defense_damage > 0.0);
  N4X_ASSERT(pd.point_defense_range_mkm > 0.0);

  // Derived design stats should include missile/PD contributions.
  const auto it_raider = content.designs.find("pirate_raider");
  N4X_ASSERT(it_raider != content.designs.end());
  const auto& raider = it_raider->second;
  N4X_ASSERT(raider.missile_damage > 0.0);
  N4X_ASSERT(raider.missile_range_mkm > 0.0);
  N4X_ASSERT(raider.missile_speed_mkm_per_day > 0.0);

  const auto it_escort = content.designs.find("escort_gamma");
  N4X_ASSERT(it_escort != content.designs.end());
  const auto& escort = it_escort->second;
  N4X_ASSERT(escort.point_defense_damage > 0.0);
  N4X_ASSERT(escort.point_defense_range_mkm > 0.0);

  // Tech unlock sanity: weapons_1 should unlock both components.
  const auto it_t = content.techs.find("weapons_1");
  N4X_ASSERT(it_t != content.techs.end());

  bool saw_missile = false;
  bool saw_pd = false;
  for (const auto& e : it_t->second.effects) {
    if (e.type != "unlock_component") continue;
    if (e.value == "missile_rack_mk1") saw_missile = true;
    if (e.value == "pd_laser_mk1") saw_pd = true;
  }
  N4X_ASSERT(saw_missile);
  N4X_ASSERT(saw_pd);

  return 0;
}
