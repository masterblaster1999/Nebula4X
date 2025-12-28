#include <iostream>

int test_date();
int test_simulation();
int test_serialization();
int test_auto_routing();
int test_order_repeat();
int test_determinism();
int test_event_export();
int test_content_validation();
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
int test_ship_repairs();
int test_fleets();

int main() {
  int fails = 0;
  fails += test_date();
  fails += test_simulation();
  fails += test_serialization();
  fails += test_auto_routing();
  fails += test_order_repeat();
  fails += test_determinism();
  fails += test_event_export();
  fails += test_content_validation();
  fails += test_random_scenario();
  fails += test_file_io();
  fails += test_json_unicode();
  fails += test_json_bom();
  fails += test_json_errors();
  fails += test_state_validation();
  fails += test_save_diff();
  fails += test_state_export();
  fails += test_fleets();
  fails += test_population_growth();
  fails += test_combat_events();
  fails += test_ship_repairs();

  if (fails == 0) {
    std::cout << "All tests passed\n";
    return 0;
  }
  std::cerr << fails << " tests failed\n";
  return 1;
}
