# Notification Center

The **Notification Center** is a persistent, triage-style inbox for important game events and alerts.

Unlike **HUD toasts** (which are temporary), the Notification Center keeps a rolling history so you can come back later and:

- review what happened
- jump to the relevant system/ship/colony
- open the Event Log or Timeline at the exact event
- drill into a Watchboard alert's JSON location

## Opening the Notification Center

You can open it in several ways:

- **View ▸ Notification Center**
- the **Inbox** button in the **status bar**
- shortcut (default): **F3** (configurable in **Settings → Hotkeys**)
- from the **Command Console** (search for “Notification Center”)

## What shows up

By default, the inbox captures:

- **Warn/Error simulation events** (the same severity that produces HUD toasts)
- **Watchboard alerts** (when a pin has alerting enabled)

Optional (can be enabled in Settings):

- **Info events**

## Triage workflow

- **Unread** items are highlighted.
- Use **Unread only** and the **text filter** to narrow the list.
- **Pin** important items so they won’t be removed by automatic retention.
- Use **Clear read** to tidy the inbox quickly.

## Jump actions

Depending on the notification type, you may see actions such as:

- **Open Log** (focuses the exact event)
- **Open Timeline** (centers the timeline on the event)
- **Open Watchboard** (focuses the alerting pin)
- **Open JSON Explorer** (jumps to the JSON pointer that changed)

## Settings

Settings live in **Settings ▸ HUD ▸ Notification Center**.

Notable options:

- capture simulation events / include info
- capture watchboard alerts
- collapse duplicates
- retention limits (max stored, days to keep)
