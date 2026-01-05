#include "ui/layout_profiles.h"

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace nebula4x::ui {
namespace {

std::string trim_copy(const std::string& s) {
  std::size_t a = 0;
  while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
  std::size_t b = s.size();
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
  return s.substr(a, b - a);
}

bool is_safe_char(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.';
}

}  // namespace

std::string sanitize_layout_profile_name(std::string name) {
  name = trim_copy(name);
  if (name.empty()) return "default";

  std::string out;
  out.reserve(name.size());
  bool last_underscore = false;
  for (char c : name) {
    if (is_safe_char(c)) {
      out.push_back(c);
      last_underscore = (c == '_');
      continue;
    }
    if (std::isspace(static_cast<unsigned char>(c))) {
      if (!out.empty() && !last_underscore) {
        out.push_back('_');
        last_underscore = true;
      }
      continue;
    }
    // Drop all other characters.
  }

  // Trim leading/trailing underscores/dots for cleanliness.
  while (!out.empty() && (out.front() == '_' || out.front() == '.')) out.erase(out.begin());
  while (!out.empty() && (out.back() == '_' || out.back() == '.')) out.pop_back();

  if (out.empty()) out = "default";
  // Keep names reasonably short (human-friendly and avoids path edge cases).
  constexpr std::size_t kMaxLen = 48;
  if (out.size() > kMaxLen) out.resize(kMaxLen);
  return out;
}

std::string make_layout_profile_ini_path(const char* dir, const std::string& profile_name) {
  const std::string safe = sanitize_layout_profile_name(profile_name);
  std::filesystem::path base = (dir && dir[0]) ? std::filesystem::path(dir) : std::filesystem::path("ui_layouts");
  std::filesystem::path p = base / (safe + ".ini");
  return p.generic_string();
}

std::vector<std::string> scan_layout_profile_names(const char* dir) {
  std::vector<std::string> out;
  std::filesystem::path base = (dir && dir[0]) ? std::filesystem::path(dir) : std::filesystem::path("ui_layouts");

  std::error_code ec;
  if (!std::filesystem::exists(base, ec) || !std::filesystem::is_directory(base, ec)) return out;

  for (const auto& ent : std::filesystem::directory_iterator(base, ec)) {
    if (ec) break;
    const auto st = ent.symlink_status(ec);
    if (ec) break;
    if (!std::filesystem::is_regular_file(st)) continue;
    const auto path = ent.path();
    if (path.extension() != ".ini") continue;
    const std::string stem = path.stem().string();
    const std::string safe = sanitize_layout_profile_name(stem);
    if (safe.empty()) continue;
    out.push_back(safe);
  }

  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  // Prefer "default" at the top if present.
  auto it = std::find(out.begin(), out.end(), "default");
  if (it != out.end() && it != out.begin()) {
    std::string tmp = *it;
    out.erase(it);
    out.insert(out.begin(), std::move(tmp));
  }
  return out;
}

bool ensure_layout_profile_dir(const char* dir, std::string* error) {
  try {
    const std::filesystem::path base = (dir && dir[0]) ? std::filesystem::path(dir) : std::filesystem::path("ui_layouts");
    std::error_code ec;
    std::filesystem::create_directories(base, ec);
    if (ec) {
      if (error) *error = ec.message();
      return false;
    }
    return true;
  } catch (const std::exception& e) {
    if (error) *error = e.what();
    return false;
  }
}

}  // namespace nebula4x::ui
