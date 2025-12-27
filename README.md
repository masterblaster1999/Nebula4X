# Nebula4X (prototype)

![CI](https://github.com/masterblaster1999/Nebula4X/actions/workflows/ci.yml/badge.svg)

Nebula4X is an **open-source, turn-based space 4X** prototype in **C++20**, inspired by the *genre* of deep, logistics-heavy games (fleet ops, industry, colonies, research).

> Not affiliated with Aurora 4X or its authors. This codebase is written from scratch; please don’t copy proprietary assets, data, or text from other games.

## What’s in this iteration

### Core simulation (`nebula4x_core`)

- Star systems, orbital bodies, ships, colonies, minerals, installations
- **Cargo holds** on ships + mineral transfer orders (prototype logistics)
- Day-based turn advancement
- Shipyard construction (optionally) consumes minerals per ton built (see `data/blueprints/starting_blueprints.json`)
- **Colony construction queue**: build installations using construction points + mineral build costs (also configured in JSON)
- Orders:
  - move-to-point / move-to-body
  - **travel via jump point** (multi-system travel)
  - **attack ship** (simple targeting)
  - **wait days** (simple scheduling)
  - **load/unload minerals** (prototype cargo logistics)
  - **repeat orders** (optional looping of a ship's queue for trade routes/patrols)
- **Jump points + multi-system state**
- **Sensors + intel**: in-system detection + last-known contact snapshots (saved)
- **Exploration**: factions track discovered star systems; entering a new system reveals it
- **Research system**:
  - tech definitions (JSON)
  - research points generation (via `research_lab` installations)
  - active research + queue
  - effects (unlock component / unlock installation)
- **Basic combat prototype**:
  - armed ships auto-fire once/day at hostiles within weapon range
  - damage + ship destruction
- JSON save/load (versioned)
- Persistent event log (saved): build/research/jump/combat notifications

### Desktop UI (`nebula4x`) — SDL2 + Dear ImGui

- Galaxy map (multi-system view)
- **Right click a system in the galaxy map** to auto-route the selected ship via jump points (**Shift queues**)
- System map (pan/zoom)
- Ship list + selection (shows HP, faction)
- **Fog-of-war** toggle (hides undetected hostiles and undiscovered systems)
- **Contacts tab**: recently seen hostiles + quick actions
- **Log tab**: view/filter/clear the saved event log; copy visible entries; export CSV
- **Jump point markers on the system map**
- **Ship tab**: quick orders (move, jump travel, attack) + cargo load/unload
  - toggle **repeat orders** for simple looping routes
- **Colony tab**: manage shipyard queue + build installations via construction queue
- **Research tab**: choose projects, queue, see progress
- **Design tab**: build custom ship designs from unlocked components

### CLI (`nebula4x_cli`)

- Headless simulation runs
- Optional save/load
- Start a new game from either the built-in **Sol** scenario or a seeded **random** scenario
- Content validation helper (`--validate-content`) for blueprint/tech modding
- Save canonicalizer (`--format-save`) to re-serialize JSON with stable ordering
- Event log dump (`--dump-events`) to print saved simulation events
- Event log export to CSV (`--export-events-csv PATH`) for spreadsheets/analysis
- Event log export to JSON (`--export-events-json PATH`) for tooling/analysis
- Event log filters: `--events-last`, `--events-category`, `--events-level`, `--events-faction`, `--events-contains`, `--events-since`, `--events-until`
- Event log summary: `--events-summary` (counts by level/category) for the filtered set
- Time warp: `--until-event N` to advance day-by-day until a new matching event occurs (defaults to warn/error; configurable via `--events-*`)
- `--quiet` to suppress non-essential summary/status output (useful for scripting)
- Script helpers: `--list-factions`, `--list-systems`, `--list-bodies`, `--list-jumps`, `--list-ships`, `--list-colonies` (print ids/names, then exit)

### Tests

- Tiny built-in runner for deterministic core logic

## Build

### Prereqs

- CMake 3.21+
- A C++20 compiler (MSVC 2022, clang 15+, gcc 12+)

Optional UI dependencies:
- SDL2 (tries `find_package(SDL2)` first; falls back to FetchContent)
- Dear ImGui (fetched via CMake for the UI target)

The core uses small in-repo utilities for JSON (save/load + content files) and logging.

### Configure + build

```bash
git clone <your repo>
cd nebula4x

# Core + tests only
cmake -S . -B build -DNEBULA4X_BUILD_UI=OFF -DNEBULA4X_BUILD_TESTS=ON
cmake --build build --config Release

# Or build UI too (requires SDL2)
cmake -S . -B build -DNEBULA4X_BUILD_UI=ON -DNEBULA4X_BUILD_TESTS=ON
cmake --build build --config Release
```

### CMake presets (optional)

If you have a recent CMake, you can use the included `CMakePresets.json`:

```bash
# Core + tests (no UI)
cmake --preset core
cmake --build --preset core --config Release
ctest --preset core -C Release

# UI + core + tests
cmake --preset ui
cmake --build --preset ui --config Release
```

Run:

```bash
# UI
./build/nebula4x

# CLI (Sol scenario)
./build/nebula4x_cli --days 30

# CLI (random scenario)
./build/nebula4x_cli --scenario random --seed 42 --systems 12 --days 30

# CLI (validate content files)
./build/nebula4x_cli --validate-content

# CLI (canonicalize a save file)
./build/nebula4x_cli --format-save --load save.json --save save_canonical.json


# CLI (dump recent warning/error events)
./build/nebula4x_cli --days 30 --dump-events --events-last 50 --events-level warn,error

# CLI (event log summary for scripts/analysis)
./build/nebula4x_cli --days 30 --events-summary --events-level warn,error

# CLI (summary of warn/error events in a date range)
./build/nebula4x_cli --days 365 --events-summary --events-level warn,error --events-since 2200-01-01 --events-until 2200-12-31

# CLI (export event log to CSV)
./build/nebula4x_cli --days 30 --export-events-csv events.csv --events-level all

# CLI (export event log to JSON)
./build/nebula4x_cli --days 30 --export-events-json events.json --events-level all

# CLI (script-friendly export, no summary output)
./build/nebula4x_cli --load save.json --quiet --export-events-csv events.csv --events-level warn,error

# CLI (list factions in a save)
./build/nebula4x_cli --load save.json --list-factions

# CLI (list systems in a save)
./build/nebula4x_cli --load save.json --list-systems

# CLI (list bodies in a save)
./build/nebula4x_cli --load save.json --list-bodies

# CLI (list jump points in a save)
./build/nebula4x_cli --load save.json --list-jumps

# CLI (list ships in a save)
./build/nebula4x_cli --load save.json --list-ships

# CLI (list colonies in a save)
./build/nebula4x_cli --load save.json --list-colonies

# CLI (time warp until next warning/error event, then save)
./build/nebula4x_cli --until-event 365 --save autosave.json

# CLI (time warp from an existing save, script-friendly)
./build/nebula4x_cli --load save.json --quiet --until-event 365 --save save_after.json

# tests
ctest --test-dir build
```

### Windows tips

Using **vcpkg** makes SDL2 detection painless:

```powershell
vcpkg install sdl2
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=<path-to-vcpkg>/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

If you ever see a linker error like `LNK2019: unresolved external symbol main` when building the UI,
it's usually SDL redefining `main` to `SDL_main`. This repo opts out of that behavior and calls
`SDL_SetMainReady()` before `SDL_Init()` so MSVC links cleanly.

## Project structure

- `include/nebula4x/` public headers
- `src/core/` simulation implementation
- `src/ui/` UI implementation
- `data/` JSON content (tech + blueprints)
- `docs/` design notes

## Next steps (suggested)

- More granular ship components (power, fuel, heat, maintenance)
- Better galaxy view + exploration mechanics (surveying, unknown exits, exploration orders)
- More combat depth (initiative, tracking, missiles, armor layers)
- Multi-faction diplomacy and intel
- Data-driven mod support (hot reload, content packs)

## License

MIT. See `LICENSE`.

## Credits

See `THIRD_PARTY_NOTICES.md` for dependency licenses.
