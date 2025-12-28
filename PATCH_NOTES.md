# Patch notes (generated 2025-12-28 r31)

This patch pack contains only the files that changed.

Apply by copying the files over the repo (drag/drop into GitHub's "Upload files"), preserving folder paths.

## Summary of changes

### r31: Fleet system (persistent ship groups) + JSON export

- Core:
  - Introduces a new persisted entity: `Fleet`.
  - Fleets belong to a faction and contain a list of ship ids + a designated leader.
  - Fleets are stored in `GameState::fleets`.
- Serialization:
  - Save version bumped to `13`.
  - Saves now include a new top-level `fleets` array (older saves without it still load).
- Validation:
  - `validate_game_state()` now validates fleet invariants:
    - faction existence,
    - ships exist and belong to the fleet's faction,
    - ships are not duplicated across fleets,
    - leader is a member (when set).
- Simulation:
  - Adds fleet management helpers (`create_fleet`, `add_ship_to_fleet`, `remove_ship_from_fleet`, etc.).
  - Adds bulk order helpers (`issue_fleet_*`) to issue orders to all ships in a fleet.
  - Automatically prunes stale fleet references on load, and keeps fleets consistent when ships are
    destroyed in combat or scrapped.
- CLI:
  - Adds `--export-fleets-json PATH` (`PATH` can be `-` for stdout).
- Utility:
  - Adds `nebula4x::fleets_to_json()` (includes per-fleet ship summaries).
- Tests:
  - Adds `test_fleets`.
  - Extends `test_state_export` with fleet JSON export coverage.

### r30: ships/colonies JSON export + validate newer order types

- CLI:
  - Adds `--export-ships-json PATH` and `--export-colonies-json PATH` (`PATH` can be `-` for stdout).
- Utility:
  - Adds deterministic JSON export helpers: `nebula4x::ships_to_json()` and `nebula4x::colonies_to_json()`.
    - Exports include resolved names (when content is available) and some computed per-day colony rates.
- Validation:
  - `validate_game_state()` now validates `OrbitBody`, `TransferCargoToShip`, and `ScrapShip` orders instead of
    flagging them as unknown.
- Tests:
  - Adds `test_state_export`.
  - Extends `test_state_validation` to cover the newer order types.

### r29: shipyard repairs for docked ships

- Simulation:
  - Ships docked at a friendly colony with at least one `shipyard` installation now automatically repair HP.
  - New `SimConfig::repair_hp_per_day_per_shipyard` (default: `0.5`).
    - Daily repair = `repair_hp_per_day_per_shipyard * shipyard_count`.
  - Logs a `Shipyard` event when a ship becomes fully repaired (to avoid event spam).
- Tests:
  - Adds `test_ship_repairs`.

### r28: colony population growth/decline (simple)

- Simulation:
  - Adds a simple daily population growth/decline model for colonies.
  - New `SimConfig::population_growth_rate_per_year` (fraction per year; default `0.01`).
    - Example: `0.01` ≈ +1%/year.
    - Negative values model population decline.
- Tests:
  - Adds `test_population_growth`.
- Docs:
  - Updates README feature list.

### r27: combat hit events (non-lethal damage logs)

- Simulation:
  - Combat now emits persistent `Combat` events when ships take damage but survive.
  - Adds `SimConfig` thresholds to control when damage events are emitted and when they are elevated to `Warn`:
    - `combat_damage_event_min_abs`
    - `combat_damage_event_min_fraction`
    - `combat_damage_event_warn_remaining_fraction`
- Tests:
  - Adds `test_combat_events`.

### r26: Save diff tooling + fix missing ship orders + scrapping refunds

- Fix (build-breaking): Restores missing `Order` variants (`OrbitBody`, `TransferCargoToShip`, `ScrapShip`) that
  were referenced by simulation/UI/serialization but absent from `orders.h`.
- Simulation:
  - Scrapping a ship at a colony now:
    - returns any carried cargo minerals to the colony stockpile, and
    - refunds a configurable fraction of shipyard mineral build costs (estimated from design mass).
  - New `SimConfig::scrap_refund_fraction` (default: 0.5).
  - Logs a persistent Shipyard event when a ship is scrapped.
- CLI:
  - Adds `--diff-saves A B` to print a deterministic structural diff between two saves.
  - Adds `--diff-saves-json PATH` (optional) to emit a machine-readable JSON diff report (`PATH` can be `-`).
- Utility:
  - Adds `nebula4x::diff_saves_to_text()` and `nebula4x::diff_saves_to_json()`.
- Tests:
  - Adds `test_save_diff`.

### r25: JSON parser BOM tolerance + CRLF-aware error line/col

- JSON:
  - `json::parse()` now ignores a UTF-8 BOM at the start of the document.
  - JSON parse error reporting now treats Windows CRLF as a **single** newline so line/column numbers
    are accurate for files edited on Windows.
- Tests:
  - Adds `test_json_bom` (parses a BOM-prefixed JSON document).
  - Extends `test_json_errors` with a CRLF regression case.

### r24: event summary CSV export (`--events-summary-csv`)

- Utility:
  - Adds `nebula4x::events_summary_to_csv()` producing a single-row CSV summary with a header.
    (Convenient for spreadsheet dashboards alongside `--export-events-csv`.)
- CLI:
  - Adds `--events-summary-csv PATH` (`PATH` can be `-` for stdout) to export the summary for the
    currently filtered event set.
  - The existing stdout-safety rules apply (only one machine-readable stdout export at a time).
- Tests:
  - Extends `test_event_export` to validate summary CSV formatting.

### r23: validate ship order references in `validate_game_state()`

- Core:
  - `validate_game_state()` now checks `ship_orders` for:
    - entries whose ship id does not exist in `ships`,
    - orders in `order_queue` / `repeat_template` that reference missing ids (bodies, jump points, target ships, colonies),
    - negative `WaitDays.days_remaining` values.
- Tests:
  - Extends `test_state_validation` to cover missing-ship ship_orders entries and invalid `MoveToBody` references.

### r22: forward-compatible ship order loading (drops unknown/invalid orders)

- Serialization:
  - Save deserialization now tolerates **unknown or malformed ship orders**.
  - Instead of aborting the entire load, invalid orders are **dropped** (with a warning logged).
- Tests:
  - Extends `test_serialization` with a regression case that injects a fake future order type and
    verifies the save still loads and the unknown order is dropped.

### r21: game state integrity validator + CLI `--validate-save`

- Core:
  - Adds `validate_game_state()` to detect broken saves / inconsistent scenarios (missing ids, mismatched cross-links, non-monotonic `next_id`/`next_event_seq`).
- CLI:
  - Adds `--validate-save` to validate the loaded (or newly created) game state and exit with a non-zero code if issues are found.
- Tests:
  - Adds `test_state_validation` to lock in expected behavior (Sol scenario is valid; injected corruptions are reported).


### r20: JSON parse errors now include line/col + a context caret

- JSON:
  - JSON parse errors now report **line/column** (1-based) in addition to the raw byte offset.
  - Includes a small context snippet of the line with a `^` caret pointing at the failure location.
    (Helps a lot when hand-editing content JSON for modding.)
- Tests:
  - Adds `test_json_errors` to verify line/col + caret formatting for a couple of common failure modes.

### r19: more robust save loading for missing metadata fields

- Serialization:
  - `deserialize_game_from_json()` now treats `save_version`, `next_id`, and `selected_system` as optional,
    making it more tolerant of very early prototypes or hand-edited saves.
  - Invalid/missing `selected_system` is repaired to `kInvalidId`.
- Tests:
  - Extends `test_serialization` to cover deserializing saves with the above keys removed.

### r18: JSON \uXXXX unicode escape support (UTF-8) + tests

- JSON:
  - The minimal JSON parser now fully decodes `\uXXXX` escapes into UTF-8.
  - Correctly handles UTF-16 **surrogate pairs** (e.g. emoji like `\uD83D\uDE80`).
  - Invalid/unpaired surrogate sequences now fail parsing instead of silently corrupting text.
- Tests:
  - Adds `test_json_unicode` to cover BMP codepoints, surrogate pairs, round-tripping, and invalid inputs.

### r17: atomic file writes for saves/exports + file I/O test

- Utility:
  - `write_text_file()` now writes via a **temporary sibling file + rename** to avoid leaving a partially
    written/truncated file behind on crash or interruption (best-effort replace on Windows).
- Tests:
  - Adds `test_file_io` to cover overwrite behavior and ensure temp siblings are not left behind.

### r16: fix CLI stdout-safe status stream

- CLI:
  - Fixes a bug where the CLI tried to choose between `std::cout` and `std::cerr` using an
    uninitialized reference (undefined behavior).
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

- Utility:
  - Adds `nebula4x::events_summary_to_json()` to generate a machine-friendly JSON summary (count, day/date range, counts by level/category).
- CLI:
  - Adds `--events-summary-json PATH` (`PATH` can be `-` for stdout) to export a JSON summary for the filtered event set.
  - When exporting machine-readable output to stdout (`PATH='-'`), non-essential status output is routed to **stderr** automatically and incompatible stdout combinations are rejected.
- Docs + tests:
  - Updates README + `docs/EVENT_LOG.md`.
  - Extends `test_event_export` to cover the summary JSON format.

### r13: event log context filters (system/ship/colony) + targeted time-warp

- Simulation:
  - Extends `EventStopCondition` with optional `system_id`, `ship_id`, and `colony_id` filters.
  - `advance_until_event` now supports stopping on events scoped to a particular system/ship/colony.
- CLI:
  - Adds new event log filters: `--events-system`, `--events-ship`, `--events-colony`.
  - These accept either a numeric id or an exact name (case-insensitive), matching `--events-faction` behavior.
  - The filters apply consistently across `--dump-events`, `--events-summary`, all event exports, and `--until-event`.
- UI:
  - Log tab adds **System / Ship / Colony** filters; Copy/Export actions respect the extended filters.
  - Auto-run (pause on event) adds the same context filters for more precise time-warping.
- Docs:
  - Updates README + `docs/EVENT_LOG.md` with the new filters.

### r12: JSONL/NDJSON event log export + script-safe stdout

- Adds `nebula4x::events_to_jsonl()` (JSON Lines / NDJSON) alongside existing CSV/JSON array exporters.
- CLI:
  - New flag: `--export-events-jsonl PATH` (supports `PATH = '-'` for stdout, like the other exporters).
  - When `--quiet` is used, the `--until-event` status line is now written to **stderr** so stdout can remain clean for machine-readable exports.
  - Guards against accidentally requesting multiple `--export-events-*` outputs to stdout at the same time.
- UI:
  - Log tab adds **Export JSONL**.
  - Export buttons now gently auto-fix the file extension when the export path looks like a common default (events.csv/json/jsonl).
- Docs:
  - Adds `docs/EVENT_LOG.md` documenting event export formats and the schema.
- Tests:
  - Extends `test_event_export` to cover JSONL output and CSV quote escaping.

### r11: UI JSON event export + shared exporter in Log tab

- UI Log tab:
  - Adds **Export JSON** (writes the currently filtered/visible events to a structured JSON array).
  - Refactors the existing **Export CSV** to use the shared `nebula4x::events_to_csv()` helper.
  - Both exports preserve the existing behavior of exporting in chronological order within the visible set.
- CLI help text + README:
  - Documents that `--export-events-csv/-json` accept `PATH = '-'` to write directly to stdout (script-friendly with `--quiet`).
- Tests:
  - Extends `test_event_export` to assert JSON output ends with a trailing newline.

### r10: fix `--export-events-json` + shared event export utils + tests

- Fixes a CLI bug where `--export-events-json PATH` only worked when `--export-events-csv` was also provided.
- Adds `nebula4x::events_to_csv()` and `nebula4x::events_to_json()` (shared helpers) under `nebula4x/util/event_export.*`.
  - The CLI now uses these helpers for both exports.
- Adds a small unit test (`test_event_export`) to prevent regressions.

### r9: CLI list bodies/jumps + export events JSON

- Adds two more script-friendly CLI helpers:
  - `--list-bodies` prints body ids/names plus basic context (type/system/orbit/position).
  - `--list-jumps` prints jump point ids/names plus system + linked destination.
- Adds `--export-events-json PATH` to export the persistent simulation event log to structured JSON.
  - Respects the same `--events-*` filters as `--dump-events` / `--export-events-csv`.
- Updates README examples to include the new helpers/JSON export.

### r8: CLI list ships/colonies + macOS CI + gitignore QoL

- Adds two more script-friendly CLI helpers:
  - `--list-ships` prints ship ids/names plus basic context (faction/system/design, hp, cargo, order queue length).
  - `--list-colonies` prints colony ids/names plus basic context (faction/system/body, population, queue sizes).
- Expands GitHub Actions CI to include **macos-latest** (core + CLI + tests; UI still OFF).
- Extends `.gitignore` with common CMake and editor artifacts (`compile_commands.json`, `.vscode/`, `cmake-build-*`, etc.).

### r7: CI + gitignore + CLI list helpers

- Adds a GitHub Actions workflow at `.github/workflows/ci.yml`.
  - Builds **core + CLI + tests** with `NEBULA4X_BUILD_UI=OFF` (no SDL2/ImGui dependency in CI).
  - Runs unit tests via `ctest` on **ubuntu-latest** and **windows-latest**.
  - Uses a Release build type/config for consistency.
- Adds a `.gitignore` for common CMake build output (`build/`, `out/`) and editor artifacts.
- Adds two small CLI quality-of-life flags:
  - `--list-factions` prints faction ids + names then exits.
  - `--list-systems` prints system ids + names (with basic counts) then exits.

### r6: CLI event log date range + summary

- Adds `--events-since X` and `--events-until X` filters for the persistent event log.
  - `X` can be either a **day number** (days since epoch) or an ISO date `YYYY-MM-DD`.
- Adds `--events-summary` to print counts by level/category for the filtered event set.
- Makes `--dump-events` / `--events-summary` / `--until-event` output avoid a leading blank line in `--quiet` mode (more script-friendly).

### r5: build fixes (MSVC)

- Fixes MSVC warning C4456 in `serialization.cpp` by avoiding variable shadowing in an `if/else if` chain.
- Fixes MSVC warning C4456 in `panels.cpp` by renaming a nested `factions` variable.
- Fixes a build break in the Design panel (`cargo_used_tons` undeclared) by defining it (designs don't carry cargo).

### CLI: time warp until a matching event (`--until-event N`)

Adds a new CLI option:

- `--until-event N`

This advances the simulation **day-by-day** up to `N` days, stopping early when a **newly recorded** persistent simulation event matches the stop criteria.

Notes:

- When `--until-event` is used, `--days` is ignored.
- Stop criteria is defined via the existing `--events-*` flags:
  - `--events-level` (defaults to `warn,error` for `--until-event` unless explicitly provided)
  - `--events-category`
  - `--events-faction`
  - `--events-contains`
- On completion, the CLI prints a single **hit / no-hit** status line (even in `--quiet` mode) so the feature is script-friendly.

### Docs

- README updated with event log analysis examples and the new `--list-*` CLI helpers.

## Compatibility notes

- **Save schema is unchanged** (still v12).
- CI and `.gitignore` changes do not affect runtime behavior.
