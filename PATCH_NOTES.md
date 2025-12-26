# Patch notes / how to apply

This patch pack contains only the files that changed.

## Changes in this patch pack

- **Persistent contacts / intel memory**
  - Factions now maintain a small `ship_contacts` map (last-seen ship snapshots).
  - Contacts are updated automatically as part of the daily tick (based on sensor detection).
  - Saves now persist contacts; save version bumped to **5**.
- **Fog-of-war is now consistent across UI**
  - A shared UI state was introduced so the **ship list**, **system map**, and **contacts tab** all respect the same FoW toggles.
  - The left sidebar no longer shows undetected hostiles when FoW is enabled.
- **System map contact overlay**
  - Shows last-known contact markers (with optional labels), configurable by max age (days).
- **New "Contacts" tab**
  - Lists known contacts, their age, last known position, and quick actions (view system / investigate / attack when detected).

## Apply locally

1. Unzip the patch pack zip.
2. Copy the extracted folders/files into your repository root (overwrite when prompted).
3. Configure and build.

## Apply via GitHub web UI

GitHub will **not** auto-extract a zip you upload.

1. Unzip the patch pack zip on your machine.
2. In your repo on GitHub: **Add file → Upload files**.
3. Drag & drop the extracted folders/files into the upload area (keep the same folder structure).
4. Commit.
