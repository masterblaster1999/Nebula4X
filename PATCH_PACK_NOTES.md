# Patch pack notes (generated 2025-12-26)

This patch pack upgrades Nebula4X with **cross-system auto-routing** for common orders,
plus basic repo hygiene (CI + editor defaults) and a regression test.

## Included changes

### Gameplay / simulation

- **New `WaitDays` ship order**:
  - Adds a simple "do nothing for N days" order that can be queued between other orders.
  - Useful for timing arrivals, pacing fleets, or delaying cargo transfers in the prototype.

- **Cross-system Move-to-Body**:
  - `Simulation::issue_move_to_body(..., restrict_to_discovered)` now auto-enqueues `TravelViaJump` steps
    so you can click a body in another system and still get there.

- **Cross-system Attack / Intercept**:
  - `Simulation::issue_attack_ship(..., restrict_to_discovered)` now auto-routes to the target’s current
    system (if detected) or last-known system (from contact memory), then executes the attack/intercept.

- **Cross-system Cargo (Load/Unload minerals)**:
  - `Simulation::issue_load_mineral(..., restrict_to_discovered)` and `issue_unload_mineral(...)` now
    auto-route to the colony’s system before performing the transfer.

- **Smarter queued travel routing**:
  - `issue_travel_to_system` now pathfinds from the ship’s *predicted* system after already-queued
    `TravelViaJump` orders (improves Shift-queue behavior).

### UI

- Adds a **Queue wait** control under **Ship → Quick orders**.
- Attack and cargo buttons now pass the current fog-of-war flag into the routing calls (so routing can be
  restricted to discovered systems when fog-of-war is enabled).
- Adds a warning log when an order can’t be queued due to no known route.

### Tests

- Adds `tests/test_auto_routing.cpp` and wires it into the test runner + CMake.

- Fixes missing includes in the test suite so CI builds cleanly.

### Repo hygiene

- Adds GitHub Actions workflow: `.github/workflows/ci.yml` (core build + tests on Linux/macOS/Windows).
- Adds `.gitignore`, `.editorconfig`, `.clang-format`.

## How to apply

### Local (recommended)

1. Unzip this patch pack.
2. Copy the extracted folders/files into your repo root (overwrite when prompted).
3. Build + run tests:
   - `cmake -S . -B build -DNEBULA4X_BUILD_UI=OFF -DNEBULA4X_BUILD_TESTS=ON`
   - `cmake --build build`
   - `ctest --test-dir build --output-on-failure`

### Using git apply

If you have the repo checked out locally:

- `git apply nebula4x_patch.diff`

### GitHub web UI

GitHub does **not** auto-extract a zip you upload.

1. Unzip locally.
2. In GitHub: **Add file → Upload files**.
3. Drag & drop the extracted folders/files (keep folder structure).
4. Commit.
