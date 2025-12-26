#include <iostream>

int test_date();
int test_simulation();
int test_serialization();

int main() {
  int fails = 0;
  fails += test_date();
  fails += test_simulation();
  fails += test_serialization();

  if (fails == 0) {
    std::cout << "All tests passed\n";
    return 0;
  }
  std::cerr << fails << " tests failed\n";
  return 1;
}
