# Guided Tours

Guided Tours are an in-game onboarding overlay designed to make a UI-heavy sandbox feel learnable.

They **do not change the simulation by themselves**. They are a UI-only layer that:

- opens the relevant windows for each step,
- spotlights the target panel,
- provides short, actionable instructions.

## Controls

- **F2** (default): Toggle the Guided Tour overlay (start/resume). You can rebind this in **Settings → Hotkeys**.
- **Left/Right Arrow**: Previous/next step (while the overlay is active).
- **Esc**: Exit the tour overlay.

You can also manage tours from **Help / Codex** (default: **F1**) → Tours.

## Tour options

The Tours tab exposes a few UI-only options:

- **Dim background**: darkens the UI outside the spotlight.
- **Block outside spotlight**: prevents accidental clicks outside the highlighted window while still letting you
  interact with the highlighted panel.
- **Pause toast popups**: hides transient event toast popups while the tour is running so the overlay stays readable.

These settings affect only your UI session.

## Included tours

The current build ships with a few starter tours:

- **Core Workspace**: Controls, Map, Details, Command Console, Notification Center, Codex.
- **Procedural Tools**: Watchboard, Data Lenses, Dashboards, UI Forge, Context Forge, OmniSearch, JSON Explorer.
- **Workspaces & Navigation**: Layout Profiles, Navigator, selection history, Settings.

More tours will be added as the simulation grows.
