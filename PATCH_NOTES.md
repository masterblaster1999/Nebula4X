# Patch notes / how to apply

This patch pack contains only the files that changed.

## Changes in this patch pack

- **Exploration: discovered star systems**
  - Factions now track discovered systems via `Faction::discovered_systems`.
  - Discovery is seeded from starting ships/colonies and updated when ships transit jump points.
  - Saves now persist discovered systems; save version bumped to **6**.
- **Jump travel quality-of-life + bugfix**
  - `TravelViaJump` now transits even if the ship is already sitting on the jump point (including the edge case of 0 speed).
  - Destination system is automatically discovered for the traveling ship's faction.
- **UI: consistent view-faction for FoW/exploration**
  - Added `UIState::viewer_faction_id` (the Research tab selection becomes the default viewer faction).
  - Left sidebar + system map will hide undiscovered systems when Fog-of-war is enabled.
  - Selecting a ship still overrides the viewer faction for detection (as before).

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
