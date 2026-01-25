# Command Console

The **Command Console** is Nebula4X's command palette.

- Open it with (default: **Ctrl+P**). You can rebind this in **Settings → Hotkeys**.
- Type to search UI actions and helper tools.
- Press **Enter** to run the highlighted command.

It is designed to reduce "panel hunting": you can jump directly to a tool window even if it is currently hidden or docked away.

## Favorites and recent commands

When the search box is empty, the Command Console shows two convenience lists:

- **Favorites**: actions you pin for one-click access.
- **Recent**: actions you ran recently (auto-updated).

To favorite an action:

1. Hover the action (from search results or browse lists)
2. Click **☆ Favorite** in the right-hand details panel

You can remove favorites from the same button (\"★ Unfavorite\"), or via the context menu on the favorite entry.

Recent commands can be cleared from the Command Console UI, and the maximum size is configurable in **Settings → HUD → Command Console**.

## Search tips

- Search is **fuzzy** and matches across:
  - command label
  - category
  - shortcut
  - keywords

- If you remember the hotkey, type part of it (e.g., `ctrl+shift+l`). The shortcut column reflects your current bindings.

## Categories

Commands are grouped by categories like:

- **Windows**: open/close UI panels
- **Tools**: inspectors, data lenses, validation helpers
- **Navigation**: quick jumps and selection helpers

Categories may evolve over time as new panels are added.

## Safety note

Most items exposed in the Command Console are **UI-only helpers**:

- They change your *workspace* (open windows, focus tabs, jump selection)
- They do not change the simulation state directly

If a command can affect simulation state, it should be clearly labeled.

