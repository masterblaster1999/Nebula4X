# Getting Started

This guide is intentionally practical: it explains how to *drive the UI* and get ships moving.

> Nebula4X is evolving quickly. If you get lost: open **Help / Codex** (default: **F1**), then press the **Command Console** (default: **Ctrl+P**) and type what you want.
>
> Tip: you can rebind common shortcuts in **Settings → Hotkeys**.

## The three core windows

Most of the gameplay loop lives in three places:

1. **Controls**: time control + debug toggles + UI helpers.
2. **Map**: navigate space; issue many orders directly from context.
3. **Details**: inspect ships/colonies/bodies; edit order queues; see logs.

Open/close them with (defaults):

- Controls: **Ctrl+1**
- Map: **Ctrl+2**
- Details: **Ctrl+3**

## Selecting things

- Use the **Directory** (default: **Ctrl+4**) for a searchable list of ships/colonies.
- Use **Navigator** (default: **Ctrl+Shift+N**) to move back/forward through selection history.

## Issuing ship orders

1. Select a ship (Directory, Map, or Command Console).
2. Open **Details** → **Ship**.
3. Find the **Orders / Queue** section.
4. Add orders (move, orbit, survey, etc.).

Tip: the ship orders UI includes a plan preview (ETA + fuel use) when the simulation can compute it.

## Running the simulation

- The main time controls live in **Controls**.
- Use small steps at first (minutes/hours) to make sure orders look correct.

## When something looks wrong

Try these tools:

- **Event Log**: Details → Log (or from the status bar)
- **State Doctor**: integrity checks and safe fixes
- **Entity Inspector**: find what references what

## Learn-by-searching

You don't need to remember window names:

1. Press the **Command Console** (default: **Ctrl+P**).
2. Type a concept ("survey", "shipyard", "trade", "watchboard").
3. Trigger the window/tool.

If you find a workflow that is awkward, consider filing an issue with a screenshot and the exact steps.
