# Patch notes / how to apply

This patch pack contains only the files that changed.

## Changes in this patch pack

- **CI: GitHub Actions build + test**
  - Added `.github/workflows/ci.yml` to configure, build, and run core tests on **Linux / Windows / macOS**.

- **Repo hygiene**
  - Added a basic `.gitignore` for common build outputs and IDE files.
  - Added `.clang-format` (referenced by `CONTRIBUTING.md`) so contributors can format consistently.

- **Shipyard QoL + bugfix**
  - `Simulation::enqueue_build(...)` now requires the colony to have at least one shipyard (prevents “stuck forever” queues).
  - Avoided accidental insertion of `"shipyard": 0` into a colony’s `installations` map during ticking/UI reads.
  - Unlock initialization now ignores installations with non-positive counts.

- **Tests**
  - Added coverage to ensure a colony without a shipyard does **not** silently gain a zero-count `"shipyard"` entry,
    and that enqueuing ship builds correctly fails without a shipyard.

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
