# Patch pack notes (generated 2025-12-27 r14)

This patch pack is designed for **GitHub web uploads** (Add file → Upload files). It contains only the files that changed since the previous patch pack.

## New in this patch pack: event summary JSON + stdout-safe mode for exports

### Utility

- Adds `nebula4x::events_summary_to_json()` to produce a JSON object summarizing a filtered event set (count, range, counts by level/category).

### CLI

- Adds `--events-summary-json PATH` (`PATH` can be `-` for stdout) to export the summary for the filtered event set.
- When exporting machine-readable output to stdout (`PATH='-'`), non-essential status output is written to **stderr** automatically so stdout stays clean.
- Guards against ambiguous stdout combinations: cannot mix stdout exports with `--dump-events`, `--events-summary`, or `--dump`.

### Docs + tests

- Updates README + `docs/EVENT_LOG.md` with the new summary export and refreshed scripting notes.
- Extends `test_event_export` to cover the summary JSON format.

## Previously added (r1-r13): event log QoL + CSV/JSON/JSONL export + time-warp helpers + initial CI

### r13: event log context filters (system/ship/colony) + targeted time-warp

#### Simulation

- Extends `EventStopCondition` with optional `system_id`, `ship_id`, and `colony_id` filters.
- `advance_until_event` now supports stopping on events scoped to a particular system/ship/colony.

#### CLI

- Adds new event log filters:
  - `--events-system` (id or exact name)
  - `--events-ship` (id or exact name)
  - `--events-colony` (id or exact name)
- These apply consistently across `--dump-events`, `--events-summary`, event exports, and `--until-event`.

#### UI

- Log tab adds **System / Ship / Colony** filters; Copy/Export actions respect them.
- Auto-run (pause on event) adds the same filters to target time-warping.

#### Docs

- Updates README + `docs/EVENT_LOG.md` with the new filters.

### r12: JSONL/NDJSON event log export + script-safe stdout

- Adds `--export-events-jsonl PATH` (supports `PATH = '-'` for stdout).
- In `--quiet` mode, the `--until-event` status line is written to **stderr** so stdout can remain clean.
- UI Log tab adds **Export JSONL**; `docs/EVENT_LOG.md` documents export formats.

### r11: UI JSON event export + shared exporter

- UI Log tab adds **Export JSON** and uses shared exporter helpers for CSV/JSON.

### r10: fix `--export-events-json` + shared event export utils + tests

- Fixes a CLI bug where `--export-events-json PATH` only worked when `--export-events-csv` was also provided.
- Adds shared helpers:
  - `nebula4x::events_to_csv()`
  - `nebula4x::events_to_json()`
- Adds a unit test (`test_event_export`) to prevent regressions.

### r7: CI + gitignore + CLI list helpers

- Adds GitHub Actions CI (ubuntu + windows).
- Adds `.gitignore` for common build/IDE artifacts.
- Adds `--list-factions` and `--list-systems` helpers.

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

#### UI: Log tab

- **Copy visible**: copies the currently filtered/visible events (last N, category, faction, search, level) to the clipboard.
- **Export CSV**: writes the currently filtered/visible event list to a CSV file (chronological order).

#### CLI: export events to CSV

- New flag: `nebula4x_cli --export-events-csv PATH`
- Respects the same filters as `--dump-events`:
  - `--events-last N`
  - `--events-category NAME`
  - `--events-level LEVEL`
  - `--events-faction X`
  - `--events-contains TEXT`

#### CLI: time warp until event

- New flag: `nebula4x_cli --until-event N`
- Advances day-by-day up to `N` days, stopping early when a **newly recorded** event matches the filters.
- Uses the existing `--events-*` flags for stop criteria.
- Defaults to `warn,error` unless `--events-level` is explicitly provided.

#### Utility

- Adds `nebula4x::csv_escape()` for correct CSV quoting/escaping.

#### Tests

- Adds a small regression check for `csv_escape()`.

## Compatibility

- **Save schema is unchanged** (still `save_version = 12`).

## How to apply (GitHub web UI)

GitHub does **not** auto-extract a zip you upload.

1. Unzip the patch pack zip locally.
2. In GitHub: **Add file → Upload files**.
3. Drag & drop the extracted folders/files (keep folder structure).
4. Commit.
