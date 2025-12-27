#pragma once

#include <string>

namespace nebula4x::log {

enum class Level { Debug = 0, Info = 1, Warn = 2, Error = 3, Off = 4 };

void set_level(Level lvl);
Level level();

void debug(const std::string& msg);
void info(const std::string& msg);
void warn(const std::string& msg);
void error(const std::string& msg);

} // namespace nebula4x::log
