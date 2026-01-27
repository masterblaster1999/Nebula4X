#pragma once
#include <cstdint>
#include <string>

namespace nebula4x {

// Days since epoch (2200-01-01). We don't care about leap seconds.
class Date {
 public:
  // Convenience constructor for UI code.
  // Equivalent to Date(days_since_epoch).
  static Date from_days_since_epoch(std::int64_t days_since_epoch) { return Date(days_since_epoch); }

  static Date from_ymd(int year, int month, int day);
  static Date parse_iso_ymd(const std::string& iso);

  Date() = default;
  explicit Date(std::int64_t days_since_epoch) : days_(days_since_epoch) {}

  std::int64_t days_since_epoch() const { return days_; }
  Date add_days(std::int64_t delta) const { return Date(days_ + delta); }

  struct YMD {
    int year;
    int month;
    int day;
  };

  YMD to_ymd() const;
  std::string to_string() const;

 private:
  std::int64_t days_{0};
};

} // namespace nebula4x
