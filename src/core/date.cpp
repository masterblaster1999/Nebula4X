#include "nebula4x/core/date.h"

#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace nebula4x {
namespace {

// Howard Hinnant's algorithms (public domain):
// https://howardhinnant.github.io/date_algorithms.html
// days_from_civil returns days since 1970-01-01.
constexpr std::int64_t days_from_civil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return static_cast<std::int64_t>(era) * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

constexpr Date::YMD civil_from_days(std::int64_t z) {
  z += 719468;
  const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const std::int64_t y = static_cast<std::int64_t>(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  const unsigned d = doy - (153 * mp + 2) / 5 + 1;
  const unsigned m = mp + (mp < 10 ? 3 : -9);
  return Date::YMD{static_cast<int>(y + (m <= 2)), static_cast<int>(m), static_cast<int>(d)};
}

constexpr std::int64_t kEpoch = days_from_civil(2200, 1, 1);

} // namespace

Date Date::from_ymd(int year, int month, int day) {
  if (month < 1 || month > 12) throw std::runtime_error("month out of range");
  if (day < 1 || day > 31) throw std::runtime_error("day out of range");
  const std::int64_t d = days_from_civil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
  return Date(d - kEpoch);
}

Date Date::parse_iso_ymd(const std::string& iso) {
  if (iso.size() != 10 || iso[4] != '-' || iso[7] != '-') {
    throw std::runtime_error("Invalid date format, expected YYYY-MM-DD: " + iso);
  }
  const int y = std::stoi(iso.substr(0, 4));
  const int m = std::stoi(iso.substr(5, 2));
  const int d = std::stoi(iso.substr(8, 2));
  return from_ymd(y, m, d);
}

Date::YMD Date::to_ymd() const {
  const std::int64_t z = days_ + kEpoch;
  return civil_from_days(z);
}

std::string Date::to_string() const {
  const auto ymd = to_ymd();
  std::ostringstream ss;
  ss << std::setfill('0') << std::setw(4) << ymd.year << '-' << std::setw(2) << ymd.month << '-' << std::setw(2)
     << ymd.day;
  return ss.str();
}

} // namespace nebula4x
