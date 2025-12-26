# Patch notes (generated 2025-12-26)

This patch pack contains only the files that changed.

## Changes in this patch pack

### Ship order automation: repeating queues (trade routes / patrol loops)

- Ships can now optionally **repeat** their order queue once it becomes empty.
- This is meant as a lightweight automation primitive for early logistics routes:
  1. Load minerals at Colony A
  2. Travel to Colony B
  3. Unload minerals at Colony B
  4. Travel back to Colony A
  5. Repeat

#### Implementation

- Added two new fields to `ShipOrders`:
  - `repeat` (bool)
  - `repeat_template` (vector of orders)
- When `repeat` is enabled and the ship's queue is empty at the start of a tick, the simulation refills the queue from `repeat_template`.
- `Clear orders` now also disables repeat and clears the template (so clearing really means "stop").

#### UI

- The **Ship** tab now shows **Repeat: ON/OFF** and provides:
  - **Enable repeat**: snapshots the current queue into the template and enables repeating.
  - **Update repeat template**: re-snapshots the current queue (repeat remains enabled).
  - **Disable repeat**: turns repeating off and clears the template.

### Save / versioning

- Scenario `save_version` bumped to **9**.
- Save/load now persists `ShipOrders.repeat` and `ShipOrders.repeat_template`.

### Docs

- `docs/ARCHITECTURE.md` updated with a short section describing repeating order queues.
- `README.md` updated to list repeat orders as a supported automation.

### Tests

- Added a regression test `test_order_repeat` covering:
  - queue refill behavior,
  - `clear_orders` disabling repeat,
  - serialization round-trip preserving repeat state/template.

## Apply locally

1. Unzip the patch pack zip.
2. Copy the extracted folders/files into your repository root (overwrite when prompted).
3. Configure/build as usual.

## Apply via GitHub web UI

1. Unzip the patch pack zip on your machine.
2. In your repo on GitHub: **Add file → Upload files**.
3. Drag & drop the extracted folders/files into the upload area (keep the same folder structure).
4. Commit changes.
