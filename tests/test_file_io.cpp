#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>

#include "nebula4x/util/file_io.h"

#define N4X_ASSERT(expr) \
  do { \
    if (!(expr)) { \
      std::cerr << "ASSERT failed: " #expr " (" << __FILE__ << ":" << __LINE__ << ")\n"; \
      return 1; \
    } \
  } while (0)

int test_file_io() {
  namespace fs = std::filesystem;

  struct CwdGuard {
    fs::path saved;
    explicit CwdGuard(fs::path p) : saved(std::move(p)) {}
    ~CwdGuard() {
      std::error_code ec_restore;
      fs::current_path(saved, ec_restore);
    }
  };

  // Prefer the system temp dir, but fall back to the repo root if not available.
  std::error_code ec;
  fs::path dir = fs::temp_directory_path(ec);
  if (ec || dir.empty()) dir = fs::path(".");

  const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
  dir /= "nebula4x_test_file_io";
  dir /= std::to_string(static_cast<long long>(nonce));

  fs::create_directories(dir, ec);
  N4X_ASSERT(!ec);

  const fs::path target = dir / "atomic.txt";

  nebula4x::write_text_file(target.string(), "hello\n");
  N4X_ASSERT(nebula4x::read_text_file(target.string()) == "hello\n");

  // Overwrite in-place; implementation should use a temp file + rename, so the end result
  // is either the old content or the new content, never a truncated partial write.
  nebula4x::write_text_file(target.string(), "world\n");
  N4X_ASSERT(nebula4x::read_text_file(target.string()) == "world\n");

  // Relative content paths should resolve from non-repo working directories too.
  const fs::path old_cwd = fs::current_path(ec);
  N4X_ASSERT(!ec);
  CwdGuard cwd_guard(old_cwd);
  fs::current_path(dir, ec);
  N4X_ASSERT(!ec);

  const std::string content_from_source_root =
      nebula4x::read_text_file("data/blueprints/starting_blueprints.json");
  N4X_ASSERT(content_from_source_root.find("\"designs\"") != std::string::npos);

  const std::string test_fixture_from_source_root =
      nebula4x::read_text_file("tests/data/content_base.json");
  N4X_ASSERT(test_fixture_from_source_root.find("\"engine_test\"") != std::string::npos);

  fs::current_path(old_cwd, ec);
  N4X_ASSERT(!ec);

  // Ensure no temp siblings are left behind.
  const std::string tmp_prefix = target.filename().string() + ".tmp";
  for (const auto& entry : fs::directory_iterator(dir)) {
    const std::string name = entry.path().filename().string();
    const bool starts_with = name.rfind(tmp_prefix, 0) == 0;
    N4X_ASSERT(!starts_with);
  }

  fs::remove_all(dir, ec);
  return 0;
}
