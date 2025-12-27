# Patch notes (generated 2025-12-27 r9)

This patch pack contains only the files that changed.

Apply by copying the files over the repo (drag/drop into GitHub's "Upload files"), preserving folder paths.

## Summary of changes

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
