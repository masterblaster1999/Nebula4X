# Architecture (v0)

## Goals

- Keep **simulation** deterministic and runnable headless.
- Keep **UI** as a thin layer over the simulation.
- Make most content **data-driven** via JSON.

## Determinism and iteration order

Most core entity stores are `std::unordered_map<Id, ...>` for ergonomics.

Because unordered-map iteration order is **not specified**, the simulation avoids relying on it when order
can change outcomes (e.g. ships competing for a limited mineral stockpile, or combat target selection when
distances are tied). Instead, these systems collect IDs and process them in a **stable sorted order**.

Save output is also kept stable by sorting per-system entity ID lists before serializing.

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
  6. Move ships (orders; `AttackShip` will chase a *last known* position if contact is lost)
  7. Update intel / contacts (sensor-based snapshots)
  8. Resolve simple combat (auto-fire + attack orders; targets must be *detected* by faction sensors)

## Docking tolerance (moving targets)

Orbital body positions update each day. For interactions with colonies/jump points, ships use a **docking range**
(see `SimConfig::docking_range_mkm`) so that a ship can remain effectively "in orbit" even when the body's
exact position changes from day to day.

This keeps prototype logistics sane (otherwise slower ships can get stuck forever chasing a planet's updated
coordinates and never transfer cargo).

## Cargo orders (prototype logistics)

- `LoadMineral` / `UnloadMineral` are **instant** when within docking range of the colony's body.
- If `tons <= 0`: perform a one-time "as much as possible" transfer and complete.
- If `tons > 0`: treat `tons` as **remaining**. Each day, transfer what you can and decrement the remaining
  amount until it reaches zero (or the ship can't make further progress due to full/empty cargo).

## Repeating order queues (prototype automation)

Ships can optionally **repeat** their order queue once it becomes empty.

- When enabled, the current queue is copied into a `repeat_template`.
- Each day, if the ship's queue is empty, the simulation refills it from the template.

This enables simple trade routes like:

1. Load minerals at Colony A
2. Travel to Colony B
3. Unload minerals at Colony B
4. Travel back to Colony A

...and then automatically loop forever.

## Sensors

- Ships provide sensors via `ShipDesign::sensor_range_mkm`.
- Colonies can provide sensors via installations with `InstallationDef::sensor_range_mkm` (e.g. `sensor_station`).
- Detection is currently **in-system** only.

## Intel / contacts

- Factions store simple contact snapshots in `Faction::ship_contacts`.
- Contacts are refreshed during the daily tick whenever a hostile is detected.
- Contacts persist for a while after they drop off sensors and are saved/loaded.

## Exploration / discovery

- Factions track which star systems they have discovered in `Faction::discovered_systems`.
- Discovery is seeded from starting ships/colonies and updated when ships transit jump points.
- The UI can optionally hide undiscovered systems when fog-of-war is enabled.
- The simulation exposes a small convenience helper, `Simulation::issue_travel_to_system`,
  that pathfinds through the known jump network and enqueues the required `TravelViaJump` steps.

## Determinism

The project aims to keep core simulation behavior deterministic across platforms.

Because many entity stores are `std::unordered_map<Id, ...>`, the simulation avoids relying on unspecified
iteration order for rules where it can affect outcomes. Where ordering matters (e.g. competing ships, multiple colonies
finishing builds), entities are processed in **stable sorted-id** order.

## Persistent event log

Key simulation events are appended to `GameState::events` during the daily tick and are saved/loaded with the game.

Each event includes:

- `seq` (monotonic event id within a save, useful for "new event" detection)
- `day` (simulation day)
- `level` (info/warn/error)
- `category` (coarse grouping for filtering)
- optional context IDs (`faction_id`, `system_id`, `ship_id`, `colony_id`, plus an optional secondary faction)
- a human-readable `message`

Examples include:

- shipyard build completion
- colony construction completion
- research completion
- jump point transit
- new system discovery
- contact gained/lost/reacquired
- ship destruction

The UI exposes this as a **Log** tab with basic filtering by level, category, and faction.

## Time warp helpers

The simulation provides `Simulation::advance_until_event(max_days, stop_condition)` to advance
day-by-day until a newly recorded event matches a filter (e.g. pause on construction completion
or on warnings/errors). The UI exposes this in the left sidebar as **Auto-run (pause on event)**.

The CLI can print it via `--dump-events`, optionally filtered:

- `--events-category NAME`
- `--events-faction X`
- `--events-last N`

## Save/load

- JSON serialization lives in `src/core/serialization.*`.
- Version field allows future migrations.

## Extending the sim

- Add new `Order` variants in `orders.*`.
- Keep rules in `Simulation` and keep UI logic out.
