# Patch notes / how to apply

This patch pack contains only the files that changed.

## Changes in this patch pack

- **Shipyard builds now consume minerals** (optional, data-driven):
  - New `build_costs_per_ton` field on the **shipyard** installation definition.
  - `tick_shipyards()` will consume the configured minerals as it builds tons each day.
  - If costs aren't configured, shipbuilding remains **free** (backwards compatible).
- **More accurate shipyard throughput**: build capacity now carries over across multiple queued ships in the same day
  (no more "overbuilding" past 0 tons remaining).
- **UI shipyard improvements**: the Colony tab now shows:
  - shipyard per-ton costs (when configured)
  - remaining mineral cost per queued ship
  - a simple "STALLED" hint when a required mineral is at 0
- **CI added**: GitHub Actions workflow builds the core + runs tests on Windows/Linux/macOS.
- **Repo hygiene**: add `.gitignore` for common build outputs and IDE files.

## Apply locally

1. Unzip `nebula4x_patch_pack.zip`.
2. Copy the extracted folders/files into your repository root (overwrite when prompted).
3. Configure and build.

## Apply via GitHub web UI

GitHub will **not** auto-extract a zip you upload.

1. Unzip `nebula4x_patch_pack.zip` on your machine.
2. In your repo on GitHub: **Add file → Upload files**.
3. Drag & drop the extracted folders/files into the upload area (keep the same folder structure).
4. Commit.
