# Intel Notebook

The **Intel Notebook** is a unified, faction-persistent knowledge base:

- **System Notes**: tagged, pinnable notes attached to star systems.
- **Journal**: curated narrative entries (including simulation-generated entries) with editing and export.

It’s designed to support *real 4X workflows*: planning, remembering, and curating what matters as the galaxy gets big.

## Open it

You can open the Intel Notebook in several ways:

- **Hotkey:** **Ctrl+Shift+I** (rebindable in *Settings → Hotkeys*).
- **View menu:** *View → Intel Notebook (Notes + Journal)*.
- **Status bar:** click **Notebook** (shows a pinned-notes indicator when available).
- **OmniSearch:** type “notebook”, “notes”, or “journal”.

## System Notes

System Notes are stored per-faction in `Faction::system_notes` and are saved with the game.

### Features

- **Search** across system name, tags, and note text.
- **Pinned only** filter.
- **Tag filter** with a tag list + usage counts.
- **Bulk tag operations** via right-click:
  - **Rename tag** across all notes.
  - **Remove tag** from all notes.
- **Editor** panel:
  - Toggle **Pinned**
  - Add/remove **tags**
  - Edit the note **text**
  - **Jump** to the system on the Galaxy Map

### Tips

- Use **Pinned** notes as an “important systems” list.
- Use tags like `#danger`, `#ruins`, `#shipyard`, `#expansion`, `#ambush` to quickly slice your intel.
- Click a tag chip in the editor to instantly filter by that tag.

### Export

- **Copy Markdown** copies the currently filtered notes to the clipboard.
- **Export Markdown…** writes a `.md` file to disk.

## Journal

The Journal is a curated log (also saved with the game) stored in `Faction::journal`.

### Features

- **Create** new entries (title, category, text).
- **Attach context** (system / ship / colony / body) based on your current selection.
- **Search** title/text.
- **Category filter**.
- **Edit** and **Delete** existing entries.
- **Jump buttons** to linked context entities.

### Capturing important events

The **Notification Center** supports **Promote to Journal**:

- Right-click a notification → **Promote to Journal**
- Or use the **Promote to Journal** button in the notification details panel

This creates a new Journal entry that carries the notification’s context IDs and opens the Notebook so you can refine it.

### Export

- **Copy Markdown** copies filtered journal entries to the clipboard.
- **Export Markdown…** writes a `.md` file to disk.

## Why this exists

Nebula4X has a lot of information density. The Intel Notebook helps you:

- turn noisy events into curated narrative,
- keep strategic context close to the map,
- and build a durable “campaign brain” that survives long sessions.
