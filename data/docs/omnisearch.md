# OmniSearch

OmniSearch is the game's **universal search + jump** tool. It can search:

- **Commands** (window toggles, navigation helpers)
- **Entities** (ships / colonies / bodies / systems + other id-bearing arrays)
- **Docs** (the in-game Codex markdown pages)
- **Live JSON** (keys/paths/scalar values) for debugging and modding

Default hotkey: **Ctrl+F** (rebind in **Settings → Hotkeys**).

## Quick start

1. Press **Ctrl+F** to open OmniSearch.
2. Type a query (e.g. `terra`, `jump point`, `shipyard`, `fuel`).
3. Press **Enter** (or double-click a row) to activate the selected result.
4. Right-click a row for context actions (copy, jump, pin to Watchboard, open in JSON Explorer, etc).

## Prefix modes

OmniSearch supports simple prefix modes:

- `>` **Commands** only (fastest)
  - Example: `> navigator`, `> time machine`
- `@` **Entities** only
  - Example: `@ terra`, `@ colony`, `@ 1024`
- `?` **Docs** only
  - Example: `? hotkeys`, `? notifications`

- `#` **UI** only (windows + layouts)
  - Example: `# production`, `# layout`, `# window manager`

### `@` with an empty query

If you type just `@` (no query), OmniSearch shows:

- your **current selection**
- your **Navigator bookmarks**
- your **recent navigation history**

This is a quick way to jump around without remembering names.

## Result types and what “activate” means

- **Command**: runs the action (opens/toggles a window, etc).
- **Window**: opens/toggles the window. Tip: hold **Shift** while activating to pop it out (floating) if supported.
- **Workspace preset**: applies a built-in workspace (window visibility) preset.
- **Layout profile**: switches to a saved docking layout profile.
- **Entity**: jumps selection to that entity (and optionally opens related windows based on Navigator settings).
- **Doc**: opens the selected page in **Help / Codex**.
- **JSON Node**: opens **JSON Explorer** at that pointer.

## Power tips

- If you're not sure *what* the sim calls something, search **Values** (for example, a specific component name).
- Use **scope toggles** (Keys / Values / Entities / Docs / Windows / Layouts) to narrow what OmniSearch scans.
- For large JSON arrays that look like “tables”, use the right-click menu:
  - **Create Data Lens**
  - **Create Dashboard**
  - **Create Pivot Table**
- If you find a useful pointer, **Pin it to the Watchboard** and optionally add an alert.
