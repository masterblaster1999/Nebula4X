#include <iostream>

int test_date();
int test_simulation();
int test_ground_ops();
int test_boarding();
int test_serialization();
int test_auto_freight();
int test_industry();
int test_refit();
int test_diplomacy();
int test_auto_routing();
int test_order_repeat();
int test_determinism();
int test_event_export();
int test_content_validation();
int test_content_overlays();
int test_spatial_index();
int test_random_scenario();
int test_file_io();
int test_json_unicode();
int test_json_bom();
int test_json_errors();
int test_state_validation();
int test_save_diff();
int test_state_export();
int test_population_growth();
int test_combat_events();
int test_shields();
int test_population_transport();
int test_ship_repairs();
int test_fleets();
int test_ai_economy();
int test_research_planner();
int test_mineral_deposits();
int test_power_system();
int test_digests();
int test_faction_economy_modifiers();

int main() {
  int fails = 0;
  fails += test_date();
  fails += test_simulation();
  fails += test_ground_ops();
  fails += test_boarding();
  fails += test_serialization();
  fails += test_auto_freight();
  fails += test_industry();
  fails += test_refit();
  fails += test_diplomacy();
  fails += test_auto_routing();
  fails += test_order_repeat();
  fails += test_determinism();
  fails += test_event_export();
  fails += test_content_validation();
  fails += test_content_overlays();
  fails += test_spatial_index();
  fails += test_random_scenario();
  fails += test_ai_economy();
  fails += test_research_planner();
  fails += test_mineral_deposits();
  fails += test_power_system();
  fails += test_digests();
  fails += test_file_io();
  fails += test_json_unicode();
  fails += test_json_bom();
  fails += test_json_errors();
  fails += test_state_validation();
  fails += test_save_diff();
  fails += test_state_export();
  fails += test_fleets();
  fails += test_population_growth();
  fails += test_population_transport();
  fails += test_combat_events();
  fails += test_shields();
  fails += test_ship_repairs();
  fails += test_faction_economy_modifiers();

  if (fails == 0) {
    std::cout << "All tests passed\n";
    return 0;
  }
  std::cerr << fails << " tests failed\n";
  return 1;
}
