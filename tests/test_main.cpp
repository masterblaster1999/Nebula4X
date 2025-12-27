#include <iostream>

int test_date();
int test_simulation();
int test_serialization();
int test_auto_routing();
int test_order_repeat();
int test_determinism();
int test_content_validation();
int test_random_scenario();

int main() {
  int fails = 0;
  fails += test_date();
  fails += test_simulation();
  fails += test_serialization();
  fails += test_auto_routing();
  fails += test_order_repeat();
  fails += test_determinism();
  fails += test_content_validation();
  fails += test_random_scenario();

  if (fails == 0) {
    std::cout << "All tests passed\n";
    return 0;
  }
  std::cerr << fails << " tests failed\n";
  return 1;
}
