#include "nebula4x/util/file_io.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace nebula4x {

namespace {

std::filesystem::path make_temp_sibling_path(const std::filesystem::path& target) {
  const auto dir = target.parent_path();
  const std::string base = target.filename().string();

  // Create a temp file name in the same directory so rename is as atomic as possible.
  // We include a timestamp and a small counter to avoid collisions.
  const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  for (int attempt = 0; attempt < 100; ++attempt) {
    std::string name = base + ".tmp." + std::to_string(now);
    if (attempt > 0) name += "." + std::to_string(attempt);
    std::filesystem::path candidate = dir.empty() ? std::filesystem::path(name) : (dir / name);
    std::error_code ec;
    if (!std::filesystem::exists(candidate, ec) && !ec) return candidate;
  }

  // Fall back to a deterministic name; collision is still extremely unlikely.
  std::string name = base + ".tmp." + std::to_string(now);
  return dir.empty() ? std::filesystem::path(name) : (dir / name);
}

struct TempFileCleanup {
  std::filesystem::path path;
  bool active{true};
  explicit TempFileCleanup(std::filesystem::path p) : path(std::move(p)) {}
  ~TempFileCleanup() {
    if (!active) return;
    std::error_code ec;
    std::filesystem::remove(path, ec);
  }
  void release() { active = false; }
};

std::filesystem::path resolve_existing_read_path(const std::filesystem::path& requested) {
  if (requested.empty()) return requested;

  std::error_code ec;
  if (requested.is_absolute()) return requested;

  if (std::filesystem::exists(requested, ec) && !ec) return requested;

  std::vector<std::filesystem::path> roots;

#ifdef NEBULA4X_SOURCE_DIR
  roots.emplace_back(NEBULA4X_SOURCE_DIR);
#endif

  ec.clear();
  std::filesystem::path cur = std::filesystem::current_path(ec);
  if (!ec && !cur.empty()) {
    for (int depth = 0; depth < 12; ++depth) {
      roots.push_back(cur);
      const auto parent = cur.parent_path();
      if (parent.empty() || parent == cur) break;
      cur = parent;
    }
  }

  for (const auto& root : roots) {
    ec.clear();
    const auto candidate = root / requested;
    if (std::filesystem::exists(candidate, ec) && !ec) {
      return candidate;
    }
  }

  return requested;
}

} // namespace

std::string read_text_file(const std::string& path) {
  const std::filesystem::path requested(path);
  const std::filesystem::path resolved = resolve_existing_read_path(requested);

  std::ifstream in(resolved, std::ios::in | std::ios::binary);
  if (!in) {
    if (resolved != requested) {
      throw std::runtime_error("Failed to open file for reading: " + path +
                               " (resolved to: " + resolved.string() + ")");
    }
    throw std::runtime_error("Failed to open file for reading: " + path);
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

void ensure_dir(const std::string& path) {
  if (path.empty()) return;
  std::error_code ec;
  std::filesystem::create_directories(path, ec);
  if (ec) {
    throw std::runtime_error("Failed to create directory: " + path + " (" + ec.message() + ")");
  }
}

void write_text_file(const std::string& path, const std::string& contents) {
  const std::filesystem::path p(path);
  if (p.has_parent_path()) ensure_dir(p.parent_path().string());

  // Write to a temp file in the same directory, then rename into place. This avoids leaving
  // behind a partially written/truncated file if the process crashes mid-write.
  const std::filesystem::path tmp = make_temp_sibling_path(p);
  TempFileCleanup cleanup(tmp);

  {
    std::ofstream out(tmp, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("Failed to open file for writing: " + tmp.string());
    out << contents;
    out.flush();
    if (!out) throw std::runtime_error("Failed to write file: " + tmp.string());
  }

  std::error_code ec;
  std::filesystem::rename(tmp, p, ec);
  if (ec) {
    // On Windows, rename generally doesn't replace existing files, so try an explicit remove.
    std::error_code rm_ec;
    std::filesystem::remove(p, rm_ec);
    ec.clear();
    std::filesystem::rename(tmp, p, ec);
  }
  if (ec) {
    throw std::runtime_error("Failed to replace file: " + path + " (" + ec.message() + ")");
  }

  cleanup.release();
}

} // namespace nebula4x
