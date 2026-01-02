#pragma once

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <string>

#include "nebula4x/core/date.h"

namespace nebula4x {

// Clamp an hour-of-day value into [0, 23].
inline int clamp_hour(int hour) { return std::clamp(hour, 0, 23); }

// Format a (whole-hour) time as "HH:00".
inline std::string format_time_hh(int hour) {
  char buf[8];
  std::snprintf(buf, sizeof(buf), "%02d:00", clamp_hour(hour));
  return std::string(buf);
}

// Format a date+hour as "YYYY-MM-DD HH:00".
inline std::string format_datetime(const Date& date, int hour) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%s %02d:00", date.to_string().c_str(), clamp_hour(hour));
  return std::string(buf);
}

// Convenience overload when you have a day number.
inline std::string format_datetime(std::int64_t day, int hour) {
  return format_datetime(Date(day), hour);
}

// Format a duration expressed in days into a human-friendly string.
//
// - For durations >= 1 day, uses days:  "1.0d".
// - For durations < 1 day, uses hours: "6.0h".
//
// This is intentionally compact for UI labels and combat event messages.
inline std::string format_duration_days(double days) {
  if (days < 0.0) days = 0.0;
  char buf[32];
  if (days >= 1.0) {
    std::snprintf(buf, sizeof(buf), "%.1fd", days);
  } else {
    std::snprintf(buf, sizeof(buf), "%.1fh", days * 24.0);
  }
  return std::string(buf);
}

} // namespace nebula4x
