#include "nebula4x/util/file_io.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace nebula4x {

std::string read_text_file(const std::string& path) {
  std::ifstream in(path, std::ios::in | std::ios::binary);
  if (!in) throw std::runtime_error("Failed to open file for reading: " + path);
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

  std::ofstream out(path, std::ios::out | std::ios::binary | std::ios::trunc);
  if (!out) throw std::runtime_error("Failed to open file for writing: " + path);
  out << contents;
  out.flush();
  if (!out) throw std::runtime_error("Failed to write file: " + path);
}

} // namespace nebula4x
