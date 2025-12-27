#pragma once

#include <string>

namespace nebula4x {

// Reads entire file into a string. Throws std::runtime_error on failure.
std::string read_text_file(const std::string& path);

// Writes string to file, creating parent directories if needed.
void write_text_file(const std::string& path, const std::string& contents);

// Creates directory (and parents) if needed; no-op if exists.
void ensure_dir(const std::string& path);

} // namespace nebula4x
