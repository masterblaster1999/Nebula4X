# Round 8 Notes

- Added `ship_orders_is_idle_for_automation()` in `include/nebula4x/core/orders.h` so planners/automation can reliably detect when a ship is truly idle (accounts for suspended orders and active repeat templates that will refill the queue).
- Updated repeat-aware idle filtering for `require_idle` in:
  - `src/core/colonist_planner.cpp`
  - `src/core/freight_planner.cpp`
  - `src/core/mine_planner.cpp`
  - `src/core/salvage_planner.cpp`
  - `src/core/troop_planner.cpp`
- Unified idle checks in related automation helpers: `src/core/simulation_tick_ai.cpp`, `src/core/contract_planner.cpp`, `src/core/fuel_planner.cpp`, `src/core/maintenance_planner.cpp`, `src/core/repair_planner.cpp`, `src/core/ai_economy.cpp`.
- Extended tests to cover repeat-vs-automation interactions: `tests/test_order_repeat.cpp`, `tests/test_freight_planner.cpp`, `tests/test_auto_mine.cpp`, `tests/test_auto_salvage.cpp`.

Verification:
- Run tests and confirm the above pass.
- In-game: enable a ship’s order repeat, then enable auto-mine/auto-salvage/etc.; the ship should keep repeating its template and not get reassigned until the repeat completes.
