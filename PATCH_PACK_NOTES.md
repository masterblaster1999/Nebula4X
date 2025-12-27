# Patch pack notes (generated 2025-12-27 r25)

This patch pack is designed for **GitHub web uploads** (Add file → Upload files).

It is **cumulative**: it contains the union of files changed/added in all rounds so far, so you can apply it in one upload.

## New in this patch pack: BOM-tolerant JSON parsing + CRLF-aware error locations

- JSON:
  - `json::parse()` now ignores a UTF-8 BOM at the start of the document (Windows-friendly).
  - JSON parse error line/column reporting now treats CRLF as a **single** newline, fixing incorrect
    line counts for Windows-edited files.
- Tests:
  - Adds `test_json_bom` and extends `test_json_errors` with a CRLF regression case.

## New in this patch pack: event summary CSV export

- Utility:
  - Adds `nebula4x::events_summary_to_csv()` (single-row CSV summary with a header).
- CLI:
  - Adds `--events-summary-csv PATH` (`PATH` can be `-` for stdout) to export the summary for the
    currently filtered event set.
  - Existing stdout-safety rules apply (only one machine-readable stdout export at a time).
- Tests:
  - Extends `test_event_export` to cover the CSV summary format.

## New in this patch pack: validate ship order references in state validation

- Core:
  - `validate_game_state()` now validates `ship_orders`:
    - flags entries for missing ship ids, and
    - flags orders in `order_queue` / `repeat_template` that refer to missing ids (body/jump/ship/colony).
- Tests:
  - Extends `test_state_validation` with regression coverage for invalid ship orders.

## New in this patch pack: forward-compatible ship order loading

- Serialization:
  - Save deserialization now tolerates unknown/malformed **ship orders**.
  - Invalid order entries are **dropped** instead of aborting the whole load.
  - A warning is logged (first few details + a summary count) to aid debugging.
- Tests:
  - Extends `test_serialization` with a regression case injecting a fake future order type.

## New in this patch pack: game state integrity validator + CLI `--validate-save`

- Core:
  - Adds `validate_game_state()` to detect broken saves / inconsistent scenarios (missing ids, mismatched cross-links, non-monotonic `next_id`/`next_event_seq`).
- CLI:
  - Adds `--validate-save` to validate the loaded/new state and exit (script-friendly; prints `State OK` on success).
- Tests:
  - Adds `test_state_validation` to cover a valid Sol scenario and a few injected corruption cases.


## New in this patch pack: JSON parse errors now include line/col + a context caret

- JSON:
  - Parse errors now report **line/column** (1-based) in addition to the raw byte offset.
  - Includes a small snippet of the line around the error with a `^` caret pointing at the failure location.
- Tests:
  - Adds `test_json_errors` to lock in the line/col + caret formatting for a couple of common failure cases.

## Previously added (r1-r19): more robust save loading for missing metadata fields

- Serialization:
  - `deserialize_game_from_json()` now treats `save_version`, `next_id`, and `selected_system` as **optional**
    (useful for very early prototypes or hand-edited saves).
  - Missing `save_version` defaults to **1** and is promoted to the current in-memory schema version.
  - Missing/invalid `selected_system` is repaired to `kInvalidId` to avoid UI issues.
- Tests:
  - Extends `test_serialization` with a regression case that strips these keys and verifies the loader
    still succeeds and repairs `next_id`.

## Previously added (r1-r18): JSON \uXXXX unicode escapes + tests

- JSON:
  - The minimal JSON parser now fully decodes `\uXXXX` escapes into UTF-8.
  - Handles UTF-16 **surrogate pairs** correctly (e.g. emoji sequences).
  - Invalid/unpaired surrogate sequences now fail parsing instead of silently corrupting text.
- Tests:
  - Adds `test_json_unicode` to cover BMP codepoints, surrogate pairs, round-tripping, and invalid inputs.

## Previously added (r1-r17): event log QoL + exports + time-warp helpers + CI + validation + atomic writes

### r17: atomic file writes + file I/O test

- Utility:
  - `write_text_file()` now writes via a **temporary sibling file + rename** to avoid leaving a partially
    written/truncated file behind on crash or interruption (best-effort replace on Windows).
- Tests:
  - Adds `test_file_io` to cover overwrite behavior and ensure temp siblings are not left behind.

### r16: fix CLI stdout-safe status stream

- CLI:
  - Fixes a bug where the CLI tried to choose between `std::cout` and `std::cerr` using an uninitialized
    reference (undefined behavior).
  - Status output now correctly goes to **stderr** when a machine-readable export is sent to stdout
    (`PATH='-'`), and to **stdout** otherwise.

### r15: tech tree cycle detection + stricter tech validation

- Content validation:
  - Detects prerequisite **cycles** in the tech tree (prevents research deadlocks).
  - Flags self-referential prerequisites and empty prereq ids.
  - Adds basic checks for empty tech names and malformed effects (empty type/value).
- Tests:
  - Extends `test_content_validation` to cover cycle detection.
### r14: event summary JSON + stdout-safe mode for exports

- Adds `nebula4x::events_summary_to_json()` and CLI `--events-summary-json`.
- Keeps stdout clean for machine-readable exports by routing status output to stderr when needed.
- Extends `test_event_export` to cover the summary JSON format.


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
