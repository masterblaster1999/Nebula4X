#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace nebula4x {

struct GameState;

// Configuration for the rolling autosave system.
//
// Autosaves are intended to be:
//  - crash-safe (uses write_text_file's temp+rename strategy)
//  - Windows-friendly (no ':' in filenames)
//  - bounded (keeps the newest N matching files)
struct AutosaveConfig {
  bool enabled{true};

  // Minimum simulated time between autosaves.
  // Values <= 0 disable autosaving.
  int interval_hours{24};

  // How many autosaves to keep (newest first).
  // Values <= 0 disable pruning.
  int keep_files{12};

  // Directory where autosaves are written.
  std::string dir{"saves/autosaves"};

  // Filename prefix (e.g. "autosave_").
  std::string prefix{"autosave_"};

  // File extension (including dot). Defaults to JSON saves.
  std::string extension{".json"};
};

struct AutosaveInfo {
  std::string path;
  std::string filename;
  std::uintmax_t size_bytes{0};
};

struct AutosaveScanResult {
  bool ok{false};
  std::string error;
  std::vector<AutosaveInfo> files;
};

struct AutosaveResult {
  bool saved{false};
  std::string path;
  int pruned{0};
  std::string error;
};

// Scan a directory for autosaves matching config.prefix and config.extension.
// Returns newest-first.
AutosaveScanResult scan_autosaves(const AutosaveConfig& cfg, int max_files = 32);

// Prune autosaves beyond cfg.keep_files. Returns number of files removed.
// Returns -1 on failure and optionally fills error.
int prune_autosaves(const AutosaveConfig& cfg, std::string* error = nullptr);

// Tracks time since last autosave and writes new snapshots when the interval elapses.
class AutosaveManager {
 public:
  AutosaveManager() = default;

  // Clears the internal "last autosaved" marker.
  void reset();

  // Conditionally autosave based on cfg.interval_hours.
  //
  // serialize_json must return a complete save-game JSON string.
  AutosaveResult maybe_autosave(const GameState& state, const AutosaveConfig& cfg,
                               const std::function<std::string()>& serialize_json);

  // Unconditionally write an autosave snapshot.
  AutosaveResult force_autosave(const GameState& state, const AutosaveConfig& cfg,
                               const std::function<std::string()>& serialize_json);

  // Introspection helpers (useful for UI status text).
  const std::string& last_autosave_path() const { return last_path_; }
  std::int64_t last_autosave_total_hours() const { return last_total_hours_; }

 private:
  std::int64_t last_total_hours_{-1};
  std::string last_path_;
};

} // namespace nebula4x
