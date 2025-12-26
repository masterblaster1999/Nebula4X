# Architecture (v0)

## Goals

- Keep **simulation** deterministic and runnable headless.
- Keep **UI** as a thin layer over the simulation.
- Make most content **data-driven** via JSON.

## Modules

- `nebula4x_core`: all game rules, entities, turn progression, save/load.
- `nebula4x` (UI): SDL2 window + Dear ImGui panels; renders state and sends commands.
- `nebula4x_cli`: small headless runner for debugging and CI.

## Tick model

- The sim advances in **whole days**. Each day:
  1. Update orbital positions (simple circular orbits)
  2. Produce minerals at colonies (based on installations)
  3. Generate research points + apply research progress
  4. Process shipyard build queues (consumes minerals per ton when configured)
  5. Process colony construction queues (installations: consumes minerals + construction points)
  6. Move ships (orders)
  7. Resolve simple combat (auto-fire + attack orders)

## Save/load

- JSON serialization lives in `src/core/serialization.*`.
- Version field allows future migrations.

## Extending the sim

- Add new `Order` variants in `orders.*`.
- Keep rules in `Simulation` and keep UI logic out.
