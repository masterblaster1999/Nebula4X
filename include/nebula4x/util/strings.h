#pragma once

#include <string>
#include <string_view>

namespace nebula4x {

std::string to_lower(std::string s);

// Case-insensitive substring search (ASCII).
// Returns true if `needle` is empty.
bool contains_ci(std::string_view haystack, std::string_view needle);

// Escapes a string for safe inclusion in a CSV cell.
//
// If the string contains a comma, quote, or newline, the result will be wrapped
// in double-quotes and any internal quotes will be doubled.
std::string csv_escape(const std::string& s);

} // namespace nebula4x
