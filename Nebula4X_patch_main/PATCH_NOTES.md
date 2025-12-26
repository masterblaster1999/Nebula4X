# Patch notes / how to apply

This patch pack contains only the files that changed.

## Changes in this patch pack (v6)

### New: cross-system Intercept / Attack auto-routing

- `Simulation::issue_attack_ship` now auto-enqueues jump travel so a ship can intercept/attack a hostile in **another system**.
- Targeting rules are unchanged: you can only issue the order if the target is currently detected **or** you have a saved `Contact` snapshot.
- `issue_attack_ship` now accepts a `restrict_to_discovered` flag (default: false) so the UI can enforce fog-of-war routing when enabled.

### UI: fog-of-war aware attack routing

- The Contacts/Ship attack buttons now pass the fog-of-war flag through to `issue_attack_ship`, matching the existing behavior for move/cargo orders.

### Tests + docs

- `tests/test_auto_routing.cpp` now covers cross-system attack auto-routing.
- `README.md` updated to mention Intercept auto-routing from the Contacts tab.

## Changes in this patch pack (v5)

### Quality of life: auto-routing for cross-system orders

- `Simulation::issue_move_to_body` now auto-enqueues jump travel so the ship can reach a body in another system.
- `Simulation::issue_load_mineral` / `Simulation::issue_unload_mineral` now auto-enqueue jump travel so cargo transfers can target colonies in other systems.
- These functions now accept a `restrict_to_discovered` flag (default: false) so the UI can enforce fog-of-war routing when enabled.

### Fix: Shift-queued travel routes now plan from the end of the queue

- `Simulation::issue_travel_to_system` now treats the ship's start system as the system it will be in after executing already-queued `TravelViaJump` orders.
- This makes Shift-queued travel routes behave intuitively (route A to B, then B to C).

### UI

- Cargo transfer panel no longer blocks cross-system transfers; it queues auto-routing travel orders.
- System map body-click move orders respect fog-of-war routing and warn if no route exists.

### Tests

- New test: `tests/test_auto_routing.cpp` validates cross-system auto-routing and end-of-queue route planning.

## Changes in this patch pack (v4)

### Fix: missing content validation files

- Adds `include/nebula4x/core/content_validation.h`
- Adds `src/core/content_validation.cpp`
- Adds `tests/test_content_validation.cpp`
- Brings the source tree back in sync with `CMakeLists.txt` and CLI includes.

### New: seeded procedural scenario

- Added `make_random_scenario(seed, num_systems)` in the scenario module.
- CLI options:
  - `--scenario sol|random` (default: sol)
  - `--seed N` (default: 1)
  - `--systems N` (default: 12)

### Tests

- New test: `tests/test_random_scenario.cpp`
  - Asserts deterministic JSON output for the same seed.
  - Validates the generated scenario against the repo content.
- CTest now runs `nebula4x_tests` with WORKING_DIRECTORY set to the repo root (so tests can read `data/...`).

### Minor

- `Simulation::cfg()` now returns a `const SimConfig&` to avoid copies.

## Changes in this patch pack (v3)

### Deterministic / canonical save serialization

- `serialize_game_to_json` now emits stable, sorted arrays for:
  - systems, bodies, jump points, ships, colonies, factions, custom designs, ship orders
  - per-system `bodies/ships/jump_points` lists
  - faction set-like lists (`known_techs`, unlock lists, `discovered_systems`) and contact arrays
- Benefit: saves are **diff-friendly** and deterministic across platforms/builds.

### CLI: `--format-save`

- New flag: `nebula4x_cli --format-save --load in.json --save out.json`
  - Rewrites a save with canonical ordering + pretty formatting and exits.

### Build convenience

- CMake now copies the repo `data/` directory next to `nebula4x_cli` after build (like the UI target).

### Tests

- Extended `tests/test_serialization.cpp` to enforce canonical ordering.


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
