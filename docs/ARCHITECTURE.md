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

## Save/load

- JSON serialization lives in `src/core/serialization.*`.
- Version field allows future migrations.

## Extending the sim

- Add new `Order` variants in `orders.*`.
- Keep rules in `Simulation` and keep UI logic out.
