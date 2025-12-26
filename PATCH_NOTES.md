# Patch notes / how to apply

This patch pack contains only the files that changed.

## Changes in this patch pack

- **Multi-jump routing (quality-of-life)**
  - Added `Simulation::issue_travel_to_system(...)` which finds a route through the jump network
    and enqueues the required `TravelViaJump` steps.
  - **UI**: In the Galaxy map, **right click** a system to route the selected ship there
    (hold **Shift** to queue instead of replacing current orders).

- **Scenario: 3-system mini-galaxy**
  - Added **Barnard's Star** as a third system, connected to Alpha Centauri via a second jump point.

- **CI: GitHub Actions build + test (now actually present)**
  - Added `.github/workflows/ci.yml` to build and run tests on Linux/Windows/macOS.

- **Tests: cleanup + new coverage**
  - Refactored `tests/test_simulation.cpp` for readability and added coverage for multi-system routing.

- **Repo hygiene**
  - Added a basic `.gitignore` for common build outputs and IDE files.

...and everything already present in the repo (cargo, construction queues, contacts, fog-of-war, etc.).

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
