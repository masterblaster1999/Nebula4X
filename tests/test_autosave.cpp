#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>

#include "nebula4x/core/game_state.h"
#include "nebula4x/util/autosave.h"
#include "nebula4x/util/file_io.h"

#define N4X_ASSERT(expr) \
  do { \
    if (!(expr)) { \
      std::cerr << "ASSERT failed: " #expr " (" << __FILE__ << ":" << __LINE__ << ")\n"; \
      return 1; \
    } \
  } while (0)

int test_autosave() {
  namespace fs = std::filesystem;

  // Prefer the system temp dir, but fall back to the repo root if not available.
  std::error_code ec;
  fs::path dir = fs::temp_directory_path(ec);
  if (ec || dir.empty()) dir = fs::path(".");

  const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
  dir /= "nebula4x_test_autosave";
  dir /= std::to_string(static_cast<long long>(nonce));

  fs::create_directories(dir, ec);
  N4X_ASSERT(!ec);

  nebula4x::GameState st;
  st.date = nebula4x::Date::from_ymd(2200, 1, 1);
  st.hour_of_day = 0;

  nebula4x::AutosaveConfig cfg;
  cfg.enabled = true;
  cfg.interval_hours = 1;
  cfg.keep_files = 3;
  cfg.dir = dir.string();
  cfg.prefix = "autosave_";
  cfg.extension = ".json";

  nebula4x::AutosaveManager mgr;

  // First call establishes baseline; no save yet.
  {
    auto r = mgr.maybe_autosave(st, cfg, [] { return std::string("{}\n"); });
    N4X_ASSERT(!r.saved);
  }

  // After +1h, should save.
  {
    st.hour_of_day = 1;
    auto r = mgr.maybe_autosave(st, cfg, [] { return std::string("{\"a\":1}\n"); });
    N4X_ASSERT(r.saved);
    N4X_ASSERT(!r.path.empty());
    N4X_ASSERT(nebula4x::read_text_file(r.path).find("\"a\":1") != std::string::npos);
  }

  // Force multiple snapshots in the same hour -> unique filenames.
  {
    st.hour_of_day = 1;
    auto r1 = mgr.force_autosave(st, cfg, [] { return std::string("{\"b\":1}\n"); });
    auto r2 = mgr.force_autosave(st, cfg, [] { return std::string("{\"c\":1}\n"); });
    N4X_ASSERT(r1.saved && r2.saved);
    N4X_ASSERT(r1.path != r2.path);
  }

  // Advance time and generate enough autosaves to require pruning.
  for (int h = 2; h <= 8; ++h) {
    st.hour_of_day = h;
    (void)mgr.maybe_autosave(st, cfg, [h] { return std::string("{\"h\":") + std::to_string(h) + "}\n"; });
  }

  // Directory should contain at most keep_files autosaves.
  {
    const auto scan = nebula4x::scan_autosaves(cfg, 100);
    N4X_ASSERT(scan.ok);
    N4X_ASSERT(static_cast<int>(scan.files.size()) <= cfg.keep_files);
    // Newest-first ordering should be stable.
    if (scan.files.size() >= 2) {
      const auto t0 = fs::last_write_time(fs::path(scan.files[0].path), ec);
      const auto t1 = fs::last_write_time(fs::path(scan.files[1].path), ec);
      if (!ec) {
        N4X_ASSERT(t0 >= t1);
      }
    }
  }

  fs::remove_all(dir, ec);
  return 0;
}
