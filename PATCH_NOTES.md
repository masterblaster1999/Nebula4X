# Patch notes (generated 2025-12-27)

This patch pack contains only the files that changed.

Apply by copying the files over the repo (drag/drop into GitHub's "Upload files"), preserving folder paths.

## Summary of changes

### Event log v5: bulk copy + CSV export

- Added **Copy visible** to copy the currently filtered/visible events (last N, category, faction, search, level) to the clipboard.
- Added **Export CSV** with an editable export path to write the visible/filtered event list to disk.
  - CSV includes both IDs and human-friendly names (faction/system/ship/colony) when available.
  - Export is written in **chronological order** (oldest to newest within the exported set).

### CLI: export events to CSV

- Added `nebula4x_cli --export-events-csv PATH` to export the persistent event log to CSV.
- The export respects the same filters as `--dump-events`:
  - `--events-last N`
  - `--events-category NAME`
  - `--events-faction X`
  - `--events-contains TEXT`

### Utility: CSV escaping helper

- Added `nebula4x::csv_escape()` for correct CSV quoting/escaping (commas, quotes, newlines).

### Tests

- Added a small test for `csv_escape()` behavior.

## Compatibility notes

- **Save schema is unchanged** (still v12 from the previous patches).
- CSV export is UI/CLI-only; it does not affect simulation determinism.
