#pragma once

// In-game documentation (Codex) browser.
//
// This is a lightweight Markdown viewer intended for quick reference inside the UI.
// It scans "data/docs" (and optionally "docs" when running from the repo) for
// Markdown files and provides:
//   - document list + filter
//   - headings table-of-contents
//   - in-document find + next/prev
//   - global search across all discovered docs
//
// The browser is rendered as an *embedded panel* (no separate ImGui::Begin/End).

namespace nebula4x::ui {

struct UIState;

void draw_docs_browser_panel(UIState& ui);

}  // namespace nebula4x::ui
