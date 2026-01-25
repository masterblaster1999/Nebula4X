# UI Tour

This is a high-level map of *where things are* in the current UI.

## Navigation patterns

- **Command Console** (default: **Ctrl+P**): fast command runner (actions + quick jumps).
- **OmniSearch** (default: **Ctrl+F**): universal search across commands, entities, docs, and live game JSON.
- **Notification Center** (default: **F3**): persistent inbox for events + watchboard alerts.
- **Docking**: all major windows are dockable.
- **Layout Profiles** (default: **Ctrl+Shift+L**): save/load complete dock layouts.

All listed shortcuts are defaults; you can rebind them in **Settings → Hotkeys**.

## Controls window (default: Ctrl+1)

- Time step buttons
- Time multiplier / warp tools
- Debug toggles (if enabled)

## Map window (default: Ctrl+2)

The Map window has multiple tabs (System / Galaxy). It is the primary place for issuing spatial orders.

### System map

- Visualizes bodies, ships, jump points, and overlays
- Context-sensitive actions for many selected objects

### Galaxy map

- Lets you change the current system quickly
- Shows routes and higher-level galaxy overlays

## Details window (default: Ctrl+3)

The Details window is where you "edit" things:

- Ship order queues
- Colony build queues / automation
- Event logs, intel, and per-entity stats

## Directory window (default: Ctrl+4)

The Directory is the "spreadsheet" view:

- filter/sort ships, colonies, bodies
- bulk-select and jump the map to selections

## Automation

- **Colony Profiles** (default: **Ctrl+Shift+B**): presets for colony behavior
- **Ship Profiles** (default: **Ctrl+Shift+M**): presets for ship behavior
- **Automation Center**: bulk automation settings, triage, and inspection

## Debug / inspection tools

These are built to make development and modding easier (and they are also useful as a player):

- Watchboard: pin JSON pointers + add alerts
- Data Lenses / Dashboards / Pivot Tables: inspect JSON arrays as tables
- UI Forge: build custom panels from JSON pointers
- State Doctor: validate/fix save integrity

If you're not sure what a tool is: press **Help / Codex** (default: **F1**) and search for it in the **Shortcuts** tab.
