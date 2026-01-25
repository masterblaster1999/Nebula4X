#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "ui/ui_state.h"

struct ImGuiIO;

namespace nebula4x::ui {

struct HotkeyDef {
  const char* id;
  const char* category;
  const char* label;
  const char* description;  // may be nullptr
  HotkeyChord default_chord;
};

// Canonical set of hotkey definitions (ids + defaults).
// UIState stores only user overrides; missing ids fall back to defaults.
const std::vector<HotkeyDef>& hotkey_defs();

// Returns the default chord for the given hotkey id, or an unbound chord if unknown.
HotkeyChord hotkey_default(std::string_view id);

// Returns the effective chord for the given hotkey id (override or default).
HotkeyChord hotkey_get(const UIState& ui, std::string_view id);

// Set an override for a hotkey.
// If the given chord matches the default chord, the override is removed.
// Returns true if UIState was modified.
bool hotkey_set(UIState& ui, std::string_view id, const HotkeyChord& chord);

// Remove any override (revert to default).
bool hotkey_reset(UIState& ui, std::string_view id);

// Clears all overrides.
void hotkeys_reset_all(UIState& ui);

// Human-friendly chord formatting. Returns an empty string for unbound hotkeys.
std::string hotkey_to_string(const HotkeyChord& chord);

// Parse a human-friendly chord string (e.g., "Ctrl+Shift+P", "F1", "Alt+Left").
// Accepts "Unbound"/"None".
bool parse_hotkey(std::string_view text, HotkeyChord* out, std::string* error = nullptr);

// Returns true if chord was pressed this frame (exact modifiers match).
// This intentionally does not trigger while typing in a text input; that policy
// is enforced at call sites.
bool hotkey_pressed(const HotkeyChord& chord, const ImGuiIO& io, bool repeat = false);

// Convenience: resolve id -> chord, then test for press.
bool hotkey_pressed(const UIState& ui, std::string_view id, const ImGuiIO& io, bool repeat = false);

// Capture a chord from live input.
// Returns true when a non-modifier key is pressed.
// If the user presses Escape with no modifiers, capture_cancelled is set.
bool capture_hotkey_chord(HotkeyChord* out, bool* capture_cancelled = nullptr);

// Export/import: newline-separated "id=Chord" text. Intended for clipboard share.
std::string export_hotkeys_text(const UIState& ui);
bool import_hotkeys_text(UIState& ui, std::string_view text, std::string* error = nullptr);

} // namespace nebula4x::ui
