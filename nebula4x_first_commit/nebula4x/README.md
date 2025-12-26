# Nebula4X (prototype)

Nebula4X is an **open-source, turn-based space 4X** prototype inspired by the *genre* of deep, logistics-heavy games (fleet ops, industry, colonies, research), built in **C++20**.

> Not affiliated with Aurora 4X or its authors. This codebase is written from scratch; please don’t copy proprietary assets, data, or text from other games.

## What’s in this first iteration

- **Core simulation library** (`nebula4x_core`) with:
  - Star systems, orbital bodies, ships, colonies, minerals, installations
  - Orders (move-to-point / move-to-body)
  - Day-based turn advancement
  - JSON save/load
- **Desktop UI** (`nebula4x`) using **SDL2 + Dear ImGui**:
  - System map (pan/zoom)
  - Colony panel (mineral production)
  - Ship list + orders
  - Turn controls (advance 1/5/30 days)
- **CLI** (`nebula4x_cli`) for headless simulation runs
- **Tests** (tiny built-in runner) for deterministic core logic

This is a foundation you can grow into something much bigger: ship design, research trees, sensors/ECM, missiles, jump points, task groups, economics, AI, etc.

## Screenshots

The UI is intentionally minimal and uses primitive shapes (no copyrighted art).

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
cmake -S . -B build -DNEBULA4X_BUILD_TESTS=ON
cmake --build build --config Release
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

## Project structure

- `include/nebula4x/` public headers
- `src/core/` simulation implementation
- `src/ui/` UI implementation
- `data/` JSON content (tech + blueprints + settings)
- `docs/` design notes

## Roadmap (suggested)

- Tech tree UI + research points
- Component-based ship design (engines, reactors, sensors, cargo)
- Combat prototype (time-step, hit resolution, damage control)
- Multiple factions + diplomacy
- Serialization versioning and modding support

## License

MIT. See `LICENSE`.

## Credits

See `THIRD_PARTY_NOTICES.md` for dependency licenses.
