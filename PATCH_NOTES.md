# Patch notes (generated 2025-12-27 r4)

This patch pack contains only the files that changed.

Apply by copying the files over the repo (drag/drop into GitHub's "Upload files"), preserving folder paths.

## Summary of changes

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

- README updated with `--until-event` examples.

## Compatibility notes

- **Save schema is unchanged** (still v12).
- No simulation determinism changes; this uses the existing `Simulation::advance_until_event()` helper.
