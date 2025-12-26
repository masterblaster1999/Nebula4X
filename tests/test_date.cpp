#include <iostream>

#include "nebula4x/core/date.h"

#define N4X_ASSERT(expr) \
  do { \
    if (!(expr)) { \
      std::cerr << "ASSERT failed: " #expr " (" << __FILE__ << ":" << __LINE__ << ")\n"; \
      return 1; \
    } \
  } while (0)

int test_date() {
  using nebula4x::Date;
  auto d = Date::from_ymd(2200, 1, 1);
  N4X_ASSERT(d.days_since_epoch() == 0);
  N4X_ASSERT(d.to_string() == "2200-01-01");

  auto d2 = Date::parse_iso_ymd("2200-12-31");
  auto ymd = d2.to_ymd();
  N4X_ASSERT(ymd.year == 2200);
  N4X_ASSERT(ymd.month == 12);
  N4X_ASSERT(ymd.day == 31);

  return 0;
}
