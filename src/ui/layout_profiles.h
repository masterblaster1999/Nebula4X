#pragma once

#include <string>
#include <vector>

namespace nebula4x::ui {

// Helpers for managing multiple Dear ImGui docking/position layouts.
//
// ImGui stores docking state and window positions in an ini file (io.IniFilename).
// This UI layer treats those ini files as user-switchable "layout profiles".

// Sanitizes a user-provided profile name into something safe for a filename.
//
// Rules:
//  - Trim whitespace.
//  - Keep [A-Za-z0-9], '-', '_', '.'
//  - Convert whitespace to '_'
//  - Drop other characters.
//  - If empty, returns "default".
std::string sanitize_layout_profile_name(std::string name);

// Computes the ini file path for a given profile name.
//
// The returned path is a simple join of: <dir>/<sanitized_profile>.ini
// If dir is empty, "ui_layouts" is used.
std::string make_layout_profile_ini_path(const char* dir, const std::string& profile_name);

// Lists available profile names (stems) by scanning <dir> for *.ini files.
// Returns sanitized stems sorted alphabetically.
std::vector<std::string> scan_layout_profile_names(const char* dir);

// Ensures <dir> exists (create_directories). Returns true on success.
bool ensure_layout_profile_dir(const char* dir, std::string* error = nullptr);

}  // namespace nebula4x::ui
