# Patch pack notes (generated 2025-12-27 r7)

This patch pack is designed for **GitHub web uploads** (Add file → Upload files). It contains only the files that changed since the previous patch pack.

## New in this patch pack: CI + gitignore + CLI list helpers

- Adds GitHub Actions CI:
  - `.github/workflows/ci.yml`
  - Builds **core + tests** with `NEBULA4X_BUILD_UI=OFF` to keep CI lightweight and avoid SDL2/ImGui dependencies.
  - Runs on **ubuntu-latest** and **windows-latest**.
- Adds `.gitignore` for common build + IDE artifacts.
- Adds two CLI helpers for scripting:
  - `--list-factions`
  - `--list-systems`

## Previously added (r1-r6): event log QoL + CSV export + time-warp CLI

### r6: CLI event log date range + summary

- Adds two new event log filters:
  - `--events-since X` (on/after)
  - `--events-until X` (on/before)
  - `X` can be either a **day number** (days since epoch) or an ISO date `YYYY-MM-DD`.
- Adds a new flag:
  - `--events-summary`
  - Prints counts by level/category for the filtered event set (respects all `--events-*` filters).

### r5: build fixes (MSVC)

- Fixes MSVC warning C4456 in `serialization.cpp` by avoiding variable shadowing in an `if/else if` chain.
- Fixes MSVC warning C4456 in `panels.cpp` by renaming a nested `factions` variable.
- Fixes a build break in the Design panel (`cargo_used_tons` undeclared) by defining it (designs don't carry cargo).

### r1-r4: event log QoL + CSV export + time-warp CLI

### UI: Log tab

- **Copy visible**: copies the currently filtered/visible events (last N, category, faction, search, level) to the clipboard.
- **Export CSV**: writes the currently filtered/visible event list to a CSV file (chronological order).

### CLI: export events to CSV

- New flag: `nebula4x_cli --export-events-csv PATH`
- Respects the same filters as `--dump-events`:
  - `--events-last N`
  - `--events-category NAME`
  - `--events-level LEVEL`
  - `--events-faction X`
  - `--events-contains TEXT`

### CLI: time warp until event

- New flag: `nebula4x_cli --until-event N`
- Advances day-by-day up to `N` days, stopping early when a **newly recorded** event matches the filters.
- Uses the existing `--events-*` flags for stop criteria.
- Defaults to `warn,error` unless `--events-level` is explicitly provided.

### Utility

- Adds `nebula4x::csv_escape()` for correct CSV quoting/escaping.

### Tests

- Adds a small regression check for `csv_escape()`.

## Compatibility

- **Save schema is unchanged** (still `save_version = 12`).

## How to apply (GitHub web UI)

GitHub does **not** auto-extract a zip you upload.

1. Unzip the patch pack zip locally.
2. In GitHub: **Add file → Upload files**.
3. Drag & drop the extracted folders/files (keep folder structure).
4. Commit.
