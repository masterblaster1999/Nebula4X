# Patch notes / how to apply

This patch pack contains only the files that changed.

## Changes in this patch pack

- **CI: GitHub Actions build + test**
  - Added a cross-platform CI workflow that configures, builds, and runs the **core + tests** (UI disabled) on **Windows, Linux, and macOS**.

- **Orders: quality-of-life helpers**
  - Added `Simulation::cancel_current_order()` and `Simulation::clear_orders()`.
  - The Ship tab now exposes **Cancel current** and **Clear orders** buttons.
  - The System map click interaction now defaults to **replace** the current order queue; hold **Shift** to **queue** additional orders.

- **System map: click-to-command improvements**
  - Clicking **near a body** issues `MoveToBody`.
  - Clicking **near a jump point** issues `TravelViaJump`.
  - Clicking empty space still issues `MoveToPoint`.

- **Contacts / intel: intercept using last-known**
  - You can now issue an `AttackShip` order even when a hostile is **not currently detected**, as long as the viewer faction has a stored **contact snapshot** in the same system.
  - The Contacts tab button now shows **Intercept** when the target is not currently detected.

## Apply locally

1. Unzip the patch pack zip.
2. Copy the extracted folders/files into your repository root (overwrite when prompted).
3. Configure and build.

## Apply via GitHub web UI

GitHub will **not** auto-extract a zip you upload.

1. Unzip the patch pack zip on your machine.
2. In your repo on GitHub: **Add file → Upload files**.
3. Drag & drop the extracted folders/files into the upload area (keep the same folder structure).
4. Commit.
