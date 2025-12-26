# Nebula4X (prototype)

Nebula4X is an **open-source, turn-based space 4X** prototype in **C++20**, inspired by the *genre* of deep, logistics-heavy games (fleet ops, industry, colonies, research).

> Not affiliated with Aurora 4X or its authors. This codebase is written from scratch; please don’t copy proprietary assets, data, or text from other games.

## What’s in this iteration

### Core simulation (`nebula4x_core`)

- Star systems, orbital bodies, ships, colonies, minerals, installations
- Day-based turn advancement
- Orders:
  - move-to-point / move-to-body
  - **travel via jump point** (multi-system travel)
  - **attack ship** (simple targeting)
- **Jump points + multi-system state**
- **Research system**:
  - tech definitions (JSON)
  - research points generation (via `research_lab` installations)
  - active research + queue
  - effects (unlock component / unlock installation)
- **Basic combat prototype**:
  - armed ships auto-fire once/day at hostiles within weapon range
  - damage + ship destruction
- JSON save/load (versioned)

### Desktop UI (`nebula4x`) — SDL2 + Dear ImGui

- System map (pan/zoom)
- Ship list + selection (shows HP, faction)
- **Jump point markers on the system map**
- **Ship tab**: quick orders (move, jump travel, attack)
- **Research tab**: choose projects, queue, see progress
- **Design tab**: build custom ship designs from unlocked components

### CLI (`nebula4x_cli`)

- Headless simulation runs
- Optional save/load

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

# CLI
./build/nebula4x_cli --days 30

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
- Better galaxy view + exploration mechanics
- More combat depth (initiative, tracking, missiles, armor layers)
- Multi-faction diplomacy and intel
- Data-driven mod support (hot reload, content packs)

## License

MIT. See `LICENSE`.

## Credits

See `THIRD_PARTY_NOTICES.md` for dependency licenses.
