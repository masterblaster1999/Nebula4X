#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace nebula4x::ui {

// Metadata for a UI command.
//
// This is intentionally UI-only (not part of the simulation). The goal is to
// provide a single source of truth that multiple surfaces can consume
// (command console/palette, OmniSearch command mode, etc.).
//
// Command ids should be stable and suitable for persistence (favorites/recent).
struct UiCommandSpec {
  std::string id;
  std::string category;
  std::string label;
  std::string tooltip;   // May be empty; falls back to label.
  std::string shortcut;  // Display string (e.g. "Ctrl+P"); may be empty.
  std::string keywords;  // Space-delimited keywords; may be empty.

  // Optional context mask (bitfield) to help surfaces present context-sensitive
  // commands. Interpretation is left to the caller (e.g. Ship/Colony/Body/System).
  std::uint32_t context_mask{0};

  // When true, the command is a toggle (the UI may show a checkmark).
  bool toggles{false};
};

// In-memory registry of UI commands.
class UiCommandRegistry {
 public:
  void clear();
  void add(UiCommandSpec spec);

  // Returns nullptr if not found.
  const UiCommandSpec* find(std::string_view id) const;

  const std::vector<UiCommandSpec>& commands() const { return commands_; }

 private:
  std::vector<UiCommandSpec> commands_;
};

// Build the default registry.
//
// NOTE: This currently returns an empty registry. It'll be populated once the
// command console finishes migrating away from the local metadata table in hud.cpp.
UiCommandRegistry build_default_ui_command_registry();

}  // namespace nebula4x::ui
