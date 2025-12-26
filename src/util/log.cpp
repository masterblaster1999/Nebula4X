#include "nebula4x/util/log.h"

#include <chrono>
#include <iostream>
#include <mutex>

namespace nebula4x::log {
namespace {
std::mutex g_mu;
Level g_level = Level::Info;

const char* label(Level l) {
  switch (l) {
    case Level::Debug: return "DEBUG";
    case Level::Info: return "INFO";
    case Level::Warn: return "WARN";
    case Level::Error: return "ERROR";
    default: return "";
  }
}

void emit(Level l, const std::string& msg) {
  if (l < g_level || g_level == Level::Off) return;
  std::lock_guard<std::mutex> lock(g_mu);
  std::cerr << "[" << label(l) << "] " << msg << "\n";
}

} // namespace

void set_level(Level lvl) { g_level = lvl; }
Level level() { return g_level; }

void debug(const std::string& msg) { emit(Level::Debug, msg); }
void info(const std::string& msg) { emit(Level::Info, msg); }
void warn(const std::string& msg) { emit(Level::Warn, msg); }
void error(const std::string& msg) { emit(Level::Error, msg); }

} // namespace nebula4x::log
