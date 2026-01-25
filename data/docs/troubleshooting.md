# Troubleshooting

## The UI feels overwhelming

- Use **Workspace presets** in the Command Console (default: **Ctrl+P**): type "workspace".
- Save a comfortable arrangement with **Layout Profiles** (default: **Ctrl+Shift+L**).

## I can't find a window/tool

Press the Command Console (default: **Ctrl+P**) and type a keyword. The console searches:

- windows (Map, Directory, Design Studio)
- tools (Watchboard, State Doctor)
- entities (ships/colonies)

## Performance is poor

- Disable heavy overlays on the Map
- Turn off event toasts if you have many events
- Use smaller windows / less docking complexity

## Something looks broken after loading

- Run **State Doctor** and validate your save
- Check the Event Log for errors

## Where are docs stored?

This Codex reads markdown from:

- `data/docs/*.md`

If you're running from the repo, it may also scan `docs/` in the project root.