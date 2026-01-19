#pragma once

#include <string>

#include "ui/ui_state.h"

namespace nebula4x::ui {

// Encode/decode a single UI Forge panel into a clipboard-friendly string.
//
// The intent is to make custom panels shareable between saves/players without
// requiring hand-editing ui_prefs.json.
//
// Format:
//   "nebula-uiforge-panel-v1 " + <compact JSON>
//
// Notes:
// - Panel/widget ids are intentionally NOT persisted in the DNA. When importing,
//   the caller should assign fresh ids using UIState::{next_ui_forge_panel_id,
//   next_ui_forge_widget_id}.
// - The decoder is tolerant: it accepts either the prefixed string or raw JSON.
std::string encode_ui_forge_panel_dna(const UiForgePanelConfig& panel);

bool decode_ui_forge_panel_dna(const std::string& text, UiForgePanelConfig* out_panel, std::string* error);

}  // namespace nebula4x::ui
