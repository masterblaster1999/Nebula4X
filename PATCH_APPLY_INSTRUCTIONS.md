# How to apply this patch (GitHub web upload)

1. Download and unzip this patch zip locally.
2. In your repo on GitHub, click **Add file → Upload files**.
3. Drag-and-drop the folders/files from this patch (e.g. `src/`, `README.md`), preserving folder paths.
   - GitHub will show the path changes; confirm they line up with your repository root.
4. Commit the changes.

Notes:
- This patch **does not change the save schema**.
- It extends the CLI event log tooling (severity filtering + `--quiet`) and updates docs to match.
