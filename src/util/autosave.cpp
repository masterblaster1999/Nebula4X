#include "nebula4x/util/autosave.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "nebula4x/core/game_state.h"
#include "nebula4x/util/file_io.h"

namespace nebula4x {
namespace {

namespace fs = std::filesystem;

bool starts_with(const std::string& s, const std::string& prefix) {
  return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

std::int64_t total_hours(const GameState& state) {
  const int h = std::clamp(state.hour_of_day, 0, 23);
  return state.date.days_since_epoch() * 24 + static_cast<std::int64_t>(h);
}

std::string two_digit(int v) {
  v = std::clamp(v, 0, 99);
  const int tens = (v / 10) % 10;
  const int ones = v % 10;
  std::string s;
  s.push_back(static_cast<char>('0' + tens));
  s.push_back(static_cast<char>('0' + ones));
  return s;
}

fs::path choose_unique_path(const fs::path& dir, const std::string& base_name, const std::string& extension) {
  fs::path p = dir / (base_name + extension);
  std::error_code ec;
  if (!fs::exists(p, ec) && !ec) return p;

  // If already exists, add a suffix.
  for (int i = 1; i < 10000; ++i) {
    fs::path cand = dir / (base_name + "_" + std::to_string(i) + extension);
    ec.clear();
    if (!fs::exists(cand, ec) && !ec) return cand;
  }

  // Very unlikely fallback: include wall-clock ticks.
  const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  return dir / (base_name + "_" + std::to_string(static_cast<long long>(now)) + extension);
}

struct ScannedFile {
  AutosaveInfo info;
  fs::file_time_type mtime;
};

AutosaveScanResult scan_autosaves_impl(const AutosaveConfig& cfg, int max_files) {
  AutosaveScanResult out;
  out.ok = false;

  if (max_files <= 0) {
    out.ok = true;
    return out;
  }

  std::error_code ec;
  fs::path dir(cfg.dir);
  if (cfg.dir.empty()) {
    out.ok = true;
    return out;
  }
  if (!fs::exists(dir, ec) || ec) {
    out.ok = true;
    return out;
  }

  std::vector<ScannedFile> files;

  for (const auto& entry : fs::directory_iterator(dir, ec)) {
    if (ec) break;
    if (!entry.is_regular_file(ec) || ec) continue;

    const fs::path p = entry.path();
    const std::string name = p.filename().string();
    if (!starts_with(name, cfg.prefix)) continue;
    if (!cfg.extension.empty() && p.extension().string() != cfg.extension) continue;

    ScannedFile f;
    f.info.path = p.string();
    f.info.filename = name;
    f.info.size_bytes = entry.file_size(ec);
    if (ec) {
      ec.clear();
      f.info.size_bytes = 0;
    }

    f.mtime = entry.last_write_time(ec);
    if (ec) {
      ec.clear();
      f.mtime = fs::file_time_type::min();
    }
    files.push_back(std::move(f));
  }

  std::sort(files.begin(), files.end(), [](const ScannedFile& a, const ScannedFile& b) {
    return a.mtime > b.mtime;
  });

  if (static_cast<int>(files.size()) > max_files) files.resize(static_cast<std::size_t>(max_files));

  out.files.reserve(files.size());
  for (auto& f : files) out.files.push_back(std::move(f.info));
  out.ok = true;
  return out;
}

int prune_autosaves_impl(const AutosaveConfig& cfg, std::string* error) {
  if (cfg.keep_files <= 0) return 0;
  if (cfg.dir.empty()) return 0;

  const AutosaveScanResult scan = scan_autosaves_impl(cfg, 1000000);
  if (!scan.ok) {
    if (error) *error = scan.error;
    return -1;
  }

  if (static_cast<int>(scan.files.size()) <= cfg.keep_files) return 0;

  int removed = 0;
  for (std::size_t i = static_cast<std::size_t>(cfg.keep_files); i < scan.files.size(); ++i) {
    std::error_code ec;
    fs::remove(fs::path(scan.files[i].path), ec);
    if (!ec) {
      ++removed;
    }
  }
  return removed;
}

AutosaveResult write_autosave_snapshot(const GameState& state, const AutosaveConfig& cfg,
                                      const std::function<std::string()>& serialize_json) {
  AutosaveResult r;

  if (cfg.dir.empty()) {
    r.error = "autosave directory is empty";
    return r;
  }
  if (cfg.prefix.empty()) {
    r.error = "autosave prefix is empty";
    return r;
  }

  const fs::path dir(cfg.dir);
  std::error_code ec;
  fs::create_directories(dir, ec);
  if (ec) {
    r.error = std::string("failed to create autosave directory: ") + ec.message();
    return r;
  }

  const std::string date_tag = state.date.to_string();
  const std::string hour_tag = two_digit(std::clamp(state.hour_of_day, 0, 23));

  // Keep filenames Windows-safe: no ':' characters.
  const std::string base_name = cfg.prefix + date_tag + "_" + hour_tag + "h";
  const fs::path path = choose_unique_path(dir, base_name, cfg.extension);

  try {
    nebula4x::write_text_file(path.string(), serialize_json());
  } catch (const std::exception& e) {
    r.error = e.what();
    return r;
  }

  r.saved = true;
  r.path = path.string();

  std::string prune_err;
  const int pruned = prune_autosaves_impl(cfg, &prune_err);
  if (pruned < 0) {
    // Snapshot succeeded; pruning is best-effort.
    r.pruned = 0;
  } else {
    r.pruned = pruned;
  }
  return r;
}

} // namespace

AutosaveScanResult scan_autosaves(const AutosaveConfig& cfg, int max_files) {
  try {
    return scan_autosaves_impl(cfg, max_files);
  } catch (const std::exception& e) {
    AutosaveScanResult out;
    out.ok = false;
    out.error = e.what();
    return out;
  }
}

int prune_autosaves(const AutosaveConfig& cfg, std::string* error) {
  try {
    return prune_autosaves_impl(cfg, error);
  } catch (const std::exception& e) {
    if (error) *error = e.what();
    return -1;
  }
}

void AutosaveManager::reset() {
  last_total_hours_ = -1;
  last_path_.clear();
}

AutosaveResult AutosaveManager::maybe_autosave(const GameState& state, const AutosaveConfig& cfg,
                                              const std::function<std::string()>& serialize_json) {
  AutosaveResult r;

  if (!cfg.enabled) return r;
  if (cfg.interval_hours <= 0) return r;

  const std::int64_t cur = total_hours(state);
  if (last_total_hours_ < 0) {
    // Establish baseline (don't autosave immediately on startup/load).
    last_total_hours_ = cur;
    return r;
  }

  // If time ever moves backwards (e.g. load earlier save), reset baseline.
  if (cur < last_total_hours_) {
    last_total_hours_ = cur;
    return r;
  }

  if (cur - last_total_hours_ < cfg.interval_hours) return r;

  r = force_autosave(state, cfg, serialize_json);
  return r;
}

AutosaveResult AutosaveManager::force_autosave(const GameState& state, const AutosaveConfig& cfg,
                                              const std::function<std::string()>& serialize_json) {
  AutosaveResult r;
  if (!cfg.enabled) return r;

  r = write_autosave_snapshot(state, cfg, serialize_json);
  if (r.saved) {
    last_total_hours_ = total_hours(state);
    last_path_ = r.path;
  }
  return r;
}

} // namespace nebula4x
