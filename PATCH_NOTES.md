# Patch notes / how to apply

This patch pack contains only the files that changed.

## Changes in this patch pack

### Gameplay / sim

- **Docking tolerance for moving bodies**
  - Added `SimConfig::docking_range_mkm` and `SimConfig::arrival_epsilon_mkm`.
  - Move-to-body, jump travel, and colony cargo interactions treat a ship as "arrived" when within docking range,
    preventing ships from getting stuck forever chasing a planet's day-to-day updated position.

- **Cargo orders can span multiple days**
  - `LoadMineral` / `UnloadMineral` with `tons > 0` now treat `tons` as **remaining**, decrementing as cargo moves.
  - If `tons <= 0`, the order still behaves as "as much as possible" and completes in one go.

### Tests

- Added a regression test that a **0-speed** ship can still complete a **multi-day load order** while docked to a moving body.

### Tooling / repo hygiene

- Added GitHub Actions CI: `.github/workflows/ci.yml` (core build + tests on Linux / macOS / Windows)
- Added `.gitignore`, `.clang-format`, and `.editorconfig`

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
