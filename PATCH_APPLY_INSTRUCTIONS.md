# How to apply this patch (GitHub web upload)

1. Download and unzip this patch zip locally.
2. In your repo on GitHub, click **Add file → Upload files**.
3. Drag-and-drop the *folders* from this patch (e.g. `include/`, `src/`, `tests/`, `docs/`) into the upload area.
   - GitHub will show the path changes; confirm they line up with your repository root.
4. Commit the changes.

Notes:
- This patch includes a save schema bump to `save_version = 10`.
- It adds a persistent event log to saves (`GameState.events`), plus a UI **Log** tab and CLI `--dump-events`.
- It also makes several colony/faction tick loops deterministic by iterating IDs in sorted order.

If you prefer git locally, you can instead unzip over your working tree and commit normally.
