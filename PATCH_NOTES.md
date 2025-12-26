# Patch notes / how to apply

This zip contains only the files that changed for the latest patch pack:

- **Fix Windows UI link error**: `LNK2019 unresolved external symbol main` when building SDL2 UI
  (SDL can rewrite `main` -> `SDL_main` on Windows). The UI now opts out via `SDL_MAIN_HANDLED` and
  calls `SDL_SetMainReady()` before `SDL_Init()`.
- Add `CMakePresets.json` for easier configure/build.
- Add a GitHub Actions CI workflow to build + run tests on Windows/Linux/macOS.
- Make save-game loading more backwards compatible by treating `shipyard_queue` as optional.
- Add a serialization regression test.

## Apply locally

1. Unzip `nebula4x_patch_pack.zip`.
2. Copy the extracted folders into your repository root (overwrite when prompted).
3. Build.

## Apply via GitHub web UI

GitHub will **not** auto-extract a zip you upload.

1. Unzip `nebula4x_patch_pack.zip` on your machine.
2. In your repo on GitHub: **Add file → Upload files**.
3. Drag & drop the extracted folders/files into the upload area (keep the same folder structure).
4. Commit.
