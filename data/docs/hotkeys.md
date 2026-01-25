# Hotkeys & Shortcuts

Nebula4X supports **rebindable hotkeys** for the most common global UI actions (opening windows, navigation, time advance, etc.).

By default, the game ships with familiar shortcuts like **Ctrl+P** (Command Console) and **F1** (Help / Codex), but you can change them to match your workflow.

## Where to edit hotkeys

1. Open **Settings** (default: **Ctrl+,**)
2. Go to the **Hotkeys** tab

From there you can:

- **Rebind** an action by clicking **Rebind…** and pressing a new key combo
- **Clear** a binding (set it to *Unbound*)
- **Reset** one binding or **Reset All** to defaults

While capturing a new binding, global shortcuts are temporarily disabled so you can press any combo without triggering actions.

## Conflicts

If two actions share the same hotkey chord, the Hotkeys tab will show a conflict indicator.

Conflicts aren’t fatal (the UI will still run), but they can lead to surprising behavior (multiple actions firing at once), so it’s best to resolve them.

## Copy / Paste hotkey sets

The Hotkeys tab includes:

- **Copy All**: copies a text block of all current bindings to your clipboard
- **Paste**: imports a text block from your clipboard

This is useful for sharing a preferred layout with friends or moving your bindings between machines.

## Persistence

Hotkeys are stored in **ui_prefs.json**:

- They are **UI-only** (not part of save-games)
- They apply to your local install

If you delete ui_prefs.json, the game will regenerate it and hotkeys will reset to defaults.
