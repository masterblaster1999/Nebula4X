## r77: Salvage research + reverse engineering

This round makes **wreck salvage** a more meaningful strategic lever:

- **Salvage research:** salvaging minerals can now award research points (RP) based on the resource's `salvage_research_rp_per_ton`.
- **Reverse engineering:** salvaging *foreign* ship wrecks accumulates reverse-engineering progress on unknown components from the wreck's source design; once progress reaches the component threshold, that component becomes **unlocked** for your faction.

UI additions:

- **Research tab:** a new **Reverse engineering** section shows active progress toward salvaged component unlocks.
- **Wreck directory:** new columns show remaining salvage RP value and (for the viewer faction) how many components in the source design are still unknown.

Tuning knobs live in `SimConfig` (`enable_salvage_research`, `salvage_research_rp_multiplier`, `enable_reverse_engineering`, etc.).


## r76: Fleet "Explore systems" mission

Adds a new fleet automation mission aimed at reducing exploration micro for groups of survey ships:

- **Survey-first option:** survey unknown jump points in the current system before transiting any surveyed exits.
- **Expansion toggle:** optionally transit surveyed exits that lead to undiscovered systems (otherwise only survey within already-discovered space).
- **Frontier routing:** if there is no local work, route to the best frontier system (weighted by remaining exploration work vs. ETA), reusing the existing exploration cache/reservation logic.

UI: Fleet details → Mission → **Explore systems**.


# Patch notes (generated 2026-01-06 r75)





## r75 (2026-01-06)

### Colony Profiles: automation presets for colony management

This round adds **Colony Profiles**, a quality-of-life feature to reduce colony micromanagement.
Profiles let you capture a colony's automation knobs (installation targets, mineral reserves/targets, garrison target) into a named preset and then apply it to other colonies—or to **all colonies** in a faction.

Highlights:

- **Core:** `ColonyAutomationProfile` + helpers to capture/apply profiles (`make_colony_profile_from_colony`, `apply_colony_profile`).
- **Savegame:** profiles persist under each faction as `colony_profiles` (JSON).
- **UI:** new **Colony Profiles** window (View menu, Tools menu, Status bar button, Command Palette).
  - Hotkey: **Ctrl+Shift+B**
  - Per-profile actions: **New / Rename / Delete / Capture / Apply / Apply-to-all**
  - Fine-grained apply toggles: installations, reserves, targets, garrison
- **Tests:** added `test_colony_profiles` to cover roundtrip serialization and apply sanitization.


## r74 (2026-01-05)

### Reference Graph 2.0: global scan mode + path finder + scalable layout

This round upgrades the **Reference Graph** window into a much more powerful navigation + debugging tool.

Highlights:

- **Global graph build mode (opt-in):** incrementally scans *all* entities in the current snapshot and builds a whole-state reference graph.
  - Tunable budgets: **entities/frame**, **scan nodes/entity**, plus **max nodes** and a new **max edges** cap.
  - Live progress indicator and pause/resume.

- **Shortest path finder** between two nodes currently present in the graph.
  - **Directed / undirected** toggle.
  - Path is highlighted directly in the canvas and also shown as a clickable list.
  - Quick set endpoints via **Shift+Click** (start) and **Ctrl+Click** (end), or via right-click context menu.

- **Neighbor panels** for the selected node.
  - Quick inbound/outbound browsing to traverse dense graphs without panning around.

- **Scale-friendly auto layout:** switches to a spatial-hash repulsion approximation on larger graphs to keep layout responsive.

- **Prefs:** UI prefs schema bumped to **27.0** (reference graph global scan knobs + max edges).



## r73 (2026-01-05)

### Watchboard Aggregates: wildcard JSON pointer queries + rollups

This round upgrades the Watchboard from single-value pins into a *tiny analytics layer*.
It introduces an opt-in JSON-pointer **glob query** (wildcards) and adds aggregate pins that can
roll up a whole set of values into one number you can chart and track over time.

Highlights:

- **Core util:** `query_json_pointer_glob(...)`
  - Supports `*` (single-segment wildcard) and `**` (recursive descent globstar) in JSON Pointer-like patterns.
  - Returns matches along with traversal stats and safety caps (`max_matches`, `max_nodes`).

- **Watchboard: Aggregate query mode**
  - Toggle any pin into **Aggregate query** mode and pick an op:
    - **Count**, **Sum**, **Avg**, **Min**, **Max**
  - Hover tooltips show match counts, numeric counts, traversal budget stats, and a small sample of matched pointers.
  - **Cached evaluation** per Game JSON cache revision (avoids re-running expensive queries every frame).
  - Query pins’ **Go** button jumps to the **first match** (and the context menu exposes copy/go helpers).

- **Prefs:**
  - Watchboard query budgets and the new per-pin query fields are persisted (schema version bumped to **26.0**).

- **Tests:**
  - Added coverage for wildcard pointer queries.

## r72 (2026-01-05)

### Time Machine: live state history + diffs (in-memory)

This round adds a new debugging + UX power tool: **Time Machine**.

It records full save-game JSON snapshots in-memory, computes compact diffs, and lets you *load* any snapshot
back into the simulation ("time travel") — ideal for debugging why a number changed, or quickly branching from
an earlier turn.

Highlights:

- **New window:** **Time Machine (State History)** (`Ctrl+Shift+D`)
  - **Recording:** auto-capture snapshots at a configurable refresh interval, or **Capture now** on demand.
  - **Ring buffer:** keep the last *N* snapshots (with a quick memory estimate shown in the UI).
  - **Diff preview:** browse changes via a fast table (op/path/before/after), with filters + op toggles.
  - **Compare modes:** compare the selected snapshot to **Prev**, or to a user-selected **Baseline**.
  - **Export / clipboard:**
    - Export full snapshot JSON.
    - Export diff report JSON.
    - Export **RFC 6902 JSON Patch** (or copy to clipboard).
  - **Integrations:**
    - Clicking a diff row jumps to the exact JSON Pointer in **JSON Explorer**.
    - Right-click diff rows to **Pin** the path to the Watchboard.

- **HUD / UX:**
  - Status bar gains a **History** button.
  - Command Palette gains an action to toggle the Time Machine window.
  - Help window lists the new shortcut.

Prefs:
- Time Machine settings are persisted to `ui_prefs.json` (schema version bumped to **25.0**).


## r71 (2026-01-05)

### Reference Graph: procedural entity-id relationship map

This round adds a new **Reference Graph** window that turns the live game-state JSON into an interactive,
entity-aware relationship graph.

Highlights:

- **New window:** **Reference Graph (Entity IDs)** (`Ctrl+Shift+G`)
  - Focus an entity id (or search by kind/name/id) and visualize its relationships as a node-link diagram.
  - **Outbound expansion:** scans the focused entity’s JSON subtree for *id-ish* references to other entities.
  - **Inbound scan:** incrementally scans the whole live snapshot for references *to* the focused id and adds edges back to
    the containing top-level entity.
  - Quality-of-life: pan/zoom canvas, drag-to-pin nodes, optional grid, force-directed auto-layout.
  - Export: **Copy Graphviz DOT** to clipboard for external graph tooling.

- **Integrations:** anywhere the UI already recognizes entity ids, you can now jump straight into the graph:
  - JSON Explorer, OmniSearch, Watchboard, Data Lenses, and Entity Inspector context actions gained **Open in Reference Graph**.

- **HUD / UX:**
  - Status bar gets a **Graph** button (seeds focus from current selection when possible).
  - Command Palette gains an action to toggle the Reference Graph window.
  - Help window lists the new shortcut.

Prefs:
- Reference Graph settings are persisted to `ui_prefs.json` (schema version bumped to **24.0**).



## r70 (2026-01-05)

### Entity Inspector + ID links (procedural debugger-grade navigation)

This round adds a new **Entity Inspector** tool and makes several existing JSON-driven windows *entity-aware*.

Key additions:

- **New window:** **Entity Inspector (ID Resolver)** (`Ctrl+G`)
  - Type/paste an entity id and instantly resolve it to a live JSON path (kind/name if available).
  - One-click actions: open in JSON Explorer, open the collection in Data Lenses, pin to Watchboard.
  - **Inbound reference scan:** incrementally scans the live game JSON for where the id appears and lists paths you can double-click to jump to.
  - Tunable perf knobs: nodes/frame + max hits, plus auto-scan toggle.

- **New shared index:** `ui/game_entity_index` builds a best-effort map of `id → {kind, name, json_pointer_path}` from the *live* game JSON snapshot.

- **Linkification / navigation upgrades (live game JSON only):**
  - **JSON Explorer:** id-like scalar values now offer “Go to referenced entity” + “Open in Entity Inspector”.
  - **Data Lenses:** id-ish columns resolve to an entity name (when possible) and become clickable; context menu gains entity actions.
  - **OmniSearch / Watchboard:** context menus now surface referenced entity actions when the selected value looks like an entity id.

- **HUD / UX polish:**
  - Status bar gets an **Entity** button.
  - Command Palette gains an action to toggle the Entity Inspector.
  - Help window lists the new shortcut.

Notes:
- Entity resolution is intentionally conservative (only “id-ish” columns/fields are linkified) to reduce false positives from numeric collisions.
- Reference scanning is incremental and capped to avoid UI hitches on very large states.



## r69 (2026-01-05)

### Shared game-state JSON cache (major perf + consistency refactor)

Several procedural/debug UI tools were independently serializing + parsing the *entire* game state into JSON.
This round introduces a **shared game JSON cache** and migrates the heaviest windows to use it.

What this changes:

- **New:** `ui/game_json_cache` — a shared cache that can be refreshed on-demand with a throttle.
  - Stores the *latest* parsed JSON snapshot as a `shared_ptr` so windows can hold stable snapshots across frames.
  - Avoids repeated full JSON parses when multiple windows refresh in the same timeframe.
  - Tracks a monotonic `revision` number for incremental builders/caches (Dashboards/Pivots/etc.).

- **Migrated windows:** Watchboard, Data Lenses, Dashboards, Pivot Tables, OmniSearch, and JSON Explorer (Current State).
  - Each tool keeps its existing per-window refresh semantics, but pulls snapshots from the shared cache.
  - Significantly reduces redundant work when multiple procedural tools are open.

No UI prefs schema changes this round.


## r68 (2026-01-05)

### OmniSearch: global fuzzy search over live game JSON

- **New window:** *OmniSearch (Game JSON)* — search keys/paths and scalar values in the live game-state JSON snapshot.
  - Incremental traversal with configurable node budget per frame + max results cap to keep the UI responsive.
  - Relevance scoring via lightweight fuzzy matching, plus key/value/case-sensitivity toggles.
  - Split-view results + details with context actions:
    - Jump to JSON Explorer at the matching JSON pointer.
    - Pin any result to Watchboard.
    - For array results: create Data Lens/Dashboard/Pivot Table from the selected path.
    - Copy JSON pointer to clipboard.

- **UX:** keyboard shortcut **Ctrl+F** + Status Bar **Search** button + Command Palette action + menu entries.

- **Prefs:** OmniSearch settings persisted to `ui_prefs.json` (schema version bump to **22.0**).

## r67 (2026-01-05)

### Pivot Tables: procedural group-by aggregations over Data Lenses

- **New window:** *Pivot Tables (Procedural Aggregations)* — build quick group-by tables over any Data Lens dataset.
  - Incremental scanning (rows/frame budget) to keep the UI responsive on large arrays.
  - Optional linkage to the lens filter (so pivots respect your current search/filter settings).
  - Group-by column + optional value aggregation (Sum/Avg/Min/Max) for simple roll-ups.
  - Drilldown: click a group to jump to the source lens filtered by that group; jump to JSON Explorer for an example row.
  - Export: copy current results as CSV to the clipboard.

- **Integrations:** create pivots directly from Data Lenses, JSON Explorer (arrays), and Watchboard items.
  - Added menu entries + HUD command palette toggle.

- **Prefs:** pivot configs persisted to `ui_prefs.json` (version bumped to 21.0).

## r66 (2026-01-05)

### Dashboards: procedural charts/widgets over Data Lenses

- UI: new **Dashboards (Procedural Charts)** window:
  - Create dashboards from existing Data Lenses or via dataset discovery (arrays-of-objects).
  - Incremental scanning (rows-per-frame) to compute column stats without freezing the UI.
  - Auto-selected widgets: KPI summary card, numeric histograms, categorical breakdown cards.
  - Drill-down actions: click category values to apply a lens filter + jump to Data Lenses; click top rows to jump to JSON Explorer.
  - Auto-grid layout adapts to window width.
- UI: integrations:
  - Data Lenses context menu: **Create Dashboard**.
  - JSON Explorer: right-click array → **Create Dashboard**; details panel adds a **Dash** shortcut.
  - Watchboard: pinned array path context menu adds **Create Dashboard**.
- Prefs: dashboards persist to `ui_prefs.json` (schema version bump to **20.0**).

## r65 (2026-01-05)

### Data Lenses: procedurally generated tables from live game JSON

- UI: new **Data Lenses (Procedural Tables)** window:
  - **Discover datasets**: scans the live game JSON for arrays-of-objects and offers one-click lens creation.
  - **Inference-driven columns**: auto-flattens nested objects into scalar columns (depth-limited) and can expose
    container sizes as numeric columns (array length / object member count).
  - **Sortable + filterable** tables with row clipping for large datasets.
  - Context actions on any cell: **copy pointer**, **copy JSON**, **pin to Watchboard**, **go to JSON Explorer**,
    and **spawn a new lens** from a nested array.
- UI: JSON Explorer integration:
  - Right-click any array node → **Create Data Lens (Procedural Table)**.
  - Selected-array details adds a **Lens** shortcut button.
- UI: Watchboard integration: right-click a pinned path that resolves to an array → **Create Data Lens**.
- Prefs: lenses persist to `ui_prefs.json` (schema version bump to **19.0**).

## r64 (2026-01-04)

### Balance Lab: round-robin design duels + Elo ratings

- Core/Util: added `duel_tournament` (round-robin duel runner) built on `duel_simulator`.
  - Supports one-way or two-way (swap sides) matchups.
  - Aggregates a full W/L/D matrix and optional Elo ratings.
  - Includes a JSON serializer (`nebula4x_duel_round_robin_v1`) for easy scripting/analysis.
- UI: new **Balance Lab** window:
  - Build a roster from content + custom designs.
  - Run a tournament incrementally (tasks-per-frame slider) with live progress.
  - View ranking table + matchup matrix.
  - Export JSON to disk or copy to clipboard.
- CLI: new `--duel-roster` mode to run the same tournament headlessly and optionally emit JSON (`--duel-roster-json`).
- Tests: added `test_duel_tournament`.

## r63 (2026-01-04)

### Metals & Minerals as first-class resources + faction ledger

- Content: added two new non-mineable **processed** resources: **Metals** and **Minerals**.
- Content: added two new installations:
  - **Metal Smelter** → consumes metal ores (Duranium/Tritanium/Boronide) to produce **Metals**.
  - **Mineral Processor** → consumes industrial minerals (Corundium/Gallicite/Uridium/Mercassium) to produce **Minerals**.
- Tech: added **Materials Processing** tech (prereq: Industrial Chemistry) that unlocks both installations.
- UI: Economy window gained a **Resources** tab: faction-level ledger of stockpiles and approximate net flow (Prod/d, Cons/d, Net/d) with category/filter controls and per-colony hover breakdown.
- Tests: added `test_materials_processing` covering input-limited processed-resource production.

## r62 (2026-01-04)

### Small QoL + correctness fixes

- UI: Economy → Mining tab now matches deposit semantics: for bodies with modeled deposits, missing minerals are treated as **absent (0)** rather than **unlimited**.
- UI: Mining depletion ETA now shows **<1 day** / **<0.1 years** when a deposit is projected to run out within the current day/year (instead of rounding to 0).
- Core: cleaned up formatting in `order_to_string()` (no logic change).

## r61 (2026-01-04)

### Resource Catalog + Weighted Mining + Multi-Mineral Economy

- Added a first-class **resource catalog** (`data/blueprints/resources.json`) and `ContentDB.resources`.
- Expanded mineral deposits to include **11 classic minerals** (plus Fuel) and seeded them in both the **Sol** and **random** scenarios.
- Mining installations now support a **generic `mining_tons_per_day`** model: extraction is distributed across a body's non-depleted deposits (weighted by remaining tons).
  - Legacy per-mineral `produces` mining is still supported for mods/tests.
- Deposit semantics tightened:
  - When deposits are **modeled** (non-empty map), missing mineral keys mean the mineral is **absent**.
  - If a body has an **empty** deposit map (legacy saves/mods), missing keys remain **unlimited** (back-compat).
- **Fuel Refineries** now consume **Sorium** to produce Fuel (input-limited).
- **Shipyards** now have **multi-mineral `build_costs_per_ton`**, so ship construction uses a mixture of minerals.
- UI: Economy window mining output is now generalized (**Mine/d** total + **Top Mine**) with a tooltip breakdown.
- Tests: added `test_resource_catalog`, and wired existing `test_mobile_mining` into the build/run.

## r60 (2026-01-03)

### Fuel Planner (Auto-tanker Preview)

- Added a new Fuel Planner window to preview (and apply) deterministic ship-to-ship refuel routes.
- Added a core fuel planner that builds multi-stop tanker routes using the same auto-tanker rules (threshold, fill target, reserve, min transfer).
- Refactored auto-tanker automation to reuse the shared fuel planner (legacy behavior: 1 dispatch per idle tanker).
- Added unit tests covering multi-stop planning, order application, and reserved-target avoidance.

## r59 (2026-01-03)

### Lead Pursuit + Intercept Solver
- Added a small `intercept.h` math helper that computes a lead/intercept aim point for constant-velocity targets while respecting a desired standoff range.
- **AttackShip** order execution now uses lead pursuit when the attacker has a velocity estimate for the target contact (based on prev/last sensor detections).
- When a contact is **lost**, AttackShip still extrapolates the last-known track *and* uses bounded lead pursuit on that predicted track (limited by remaining prediction budget), reducing tail-chasing.
- UI: Intel window attack hint updated to reflect lead pursuit behavior.
- Tests: added `test_intercept` (math) and `test_attack_lead_pursuit` (integration) to lock the behavior in.

## r58 (2026-01-03)

### Planner-driven Time Warp
- **Planner rows now have action buttons:**
  - **Warp**: advance to the forecasted day/hour (interrupts on any WARN/ERROR event).
  - **Until**: advance until a matching **INFO** event occurs (category/context-scoped).  
    *Note:* scopes WARN/ERROR too (so unrelated problems elsewhere may be ignored).
- Added **Warp to next** (top of Planner): jumps to the soonest currently-visible item (honors filters/search).

### Time Warp quick-start API
- Added a small UI API (**TimeWarpQuickStart** + **time_warp_quick_start**) so other windows can programmatically start a time warp job.
- Time Warp window can now optionally display a **target label/time** while running, and can treat **reaching the time budget as success** (used by Planner’s Warp-to-time).

This patch pack contains only the files that changed.

Apply by copying the files over the repo (drag/drop into GitHub's "Upload files"), preserving folder paths.

## Summary of changes

### r57: System map sensor coverage overlay + signature probe

- UI: new **Sensor coverage (faction)** overlay on the system map.
  - Draws combined detection rings from all sensor sources in the current system (ships + colonies), including mutual-friendly sensor sharing.
  - Respects ship **Sensor Mode** (Passive/Normal/Active) and sensor power policy (offline sensors contribute no range).

- UI: new coverage controls:
  - **Target signature** slider (lets you see how stealth/EMCON affects detectability; 1.0 = baseline).
  - **Fill** toggle for a subtle heat/coverage feel.
  - **Max sources** cap to keep the overlay fast in late-game swarms.
  - Cursor probe: shows whether a hypothetical target at the cursor would be detected for the chosen signature.

- UI prefs: persist the new overlay settings (ui_prefs schema bumped to v12).

- Tests: add `test_sensor_coverage` validating that detection:
  - scales with sensor mode range multipliers,
  - scales with target signature multiplier,
  - works via colony sensor installations even when ship sensors are disabled.

### r56: Ground battle forecast + invasion advisor + planner integration

- Core: add `ground_battle_forecast`:
  - `forecast_ground_battle(cfg, attacker, defender, forts)` mirrors the daily loss model in `tick_ground_combat`.
  - `square_law_required_attacker_strength(cfg, defender, forts, margin)` gives a quick baseline for “how many troops do I need?”.
  - Utility `ground_battle_winner_label()` for lightweight UI string output.

- Planner: optional inclusion of **ground battle resolution forecasts** (category: Combat).
  - Works for both your invasions and battles defending your colonies.
  - Adds a new planner option toggle: “Include ground battles”.

- UI:
  - Colony panel shows a small **battle forecast** when a ground battle is in progress.
  - Ship→Enemy-colony panel shows an **Invasion advisor**: fortifications, troop shortfall, and “if invade now” winner + ETA.

- Tests: add `test_ground_battle_forecast` verifying forecast matches the simulation tick model (including the defender-favored tie case).

### r55: Predictive Contact Tracking (fog-of-war pursuit)

- Core: Contacts now preserve a small **2-point track** (`prev_seen_day`, `prev_seen_position_mkm`) when they are re-detected in the same system.
  - This enables a simple constant-velocity estimate without adding a heavy track-history system.
  - Repeated detections within the same day keep the previous-day track intact.
  - System changes reset the previous snapshot to avoid cross-system extrapolation.

- Core: add `contact_prediction.h` helper (header-only):
  - `predict_contact_position(Contact, now_day, max_extrap_days)` → predicted position + estimated velocity + clamped extrapolation days.
  - New `SimConfig::contact_prediction_max_days` to clamp stale extrapolation (default: 30 days).

- Orders/Combat: smarter **lost-contact pursuit**:
  - `issue_attack_ship()` seeds `AttackShip.last_known_position_mkm` using the predicted position (instead of the raw last-seen position) when the target is not currently detected.
  - `tick_ships()` keeps updating `AttackShip.last_known_position_mkm` each tick while the contact remains lost, so ships continue chasing the *track* instead of a stale point forever.

- UI: Intel window now shows the predicted position + estimated velocity and uses the predicted position for:
  - Centering the system map / radar
  - Intercept (move)
  (Attack automatically benefits from the improved core logic.)

- Save: bump save schema to **v42** and serialize optional prev-track contact fields.

- Tests: add `test_contact_prediction` covering attack-order seeding + day-to-day last-known updates during lost-contact pursuit.

### r54: Freight Planner (auto-freight preview + one-click apply)

- Core: add `freight_planner`:
  - Deterministic **dry-run** auto-freight style planning via `compute_freight_plan()`.
  - Optional helpers to apply a single assignment or the whole plan (`apply_freight_assignment`, `apply_freight_plan`).
  - Respects existing logistics rules:
    - manual **mineral reserves**
    - per-need dynamic reserves (via `logistics_needs_for_faction`)
    - `auto_freight_min_transfer_tons`, `auto_freight_max_take_fraction_of_surplus`, and multi-mineral bundling.
  - Planning is **read-only** (does not mutate colony minerals/ship cargo).

- UI: add a new **Freight Planner** window (View → Freight Planner / Tools → Open Freight Planner / Status bar → Freight):
  - Preview planned shipments per ship (From → To, cargo list, ETA + arrival D+date).
  - Tooltips show per-item reasons when available (shipyard/construction/targets/etc).
  - One-click **Apply** per row, or **Apply all**.
  - Options: filter to auto-freight ships / idle ships, restrict routing to discovered systems, override bundling, max ships, clear orders before applying.

- Tests: add `test_freight_planner` covering:
  - basic plan generation
  - reserves capping
  - multi-mineral bundling
  - `require_auto_freight_flag` filtering

- Build: wire the new core module + UI window + test into CMake and persist `show_freight_window` in UI prefs.

### r53: UI Time Warp (advance until matching event)

- UI: add a new **Time Warp** window (View → Time Warp / Tools → Open Time Warp / Status bar → Warp).
  - Runs deterministic "warp" using `Simulation::advance_until_event_hours()`.
  - Stop conditions:
    - event level (Info/Warn/Error)
    - optional category filter
    - optional message substring filter
    - optional scope filters (faction / selected system / selected ship / selected colony)
  - Runs incrementally in chunks per frame to keep the UI responsive (configurable `chunk_hours/frame`).
  - On hit: optionally auto-opens **Timeline** and focuses the event; optionally focuses context (ship/colony/system) in Details/Map.

- Tests: add `test_time_warp` covering `advance_until_event_hours()` stopping behavior for Info vs Warn under a Research-category filter.
- Build: wire the new UI file + test into CMake and persist `show_time_warp_window` in UI prefs.

### r52: Global Planner window (unified forecast dashboard)

- Core: add `planner_events` which aggregates best-effort forecast items into a single, chronological list:
  - Research completions (via `research_schedule`)
  - Colony shipyard + construction completions (via `colony_schedule`)
  - Optional ship-order ETAs (via `order_planner`, disabled by default to keep it light)
  - Per-source and global safety caps (`max_days`, `max_items`, ship limits) with clear truncation reasons.

- UI: add a new **Planner** window (View → Planner / Tools → Open Planner):
  - Faction selector
  - Auto-refresh (on time advance), with explicit “Refresh now” button
  - Search + category/level filters
  - Click an item to jump to the relevant ship/colony/system and open the right Details tab.

- Tests: add `test_planner_events` to ensure deterministic ordering when multiple forecast categories complete at the same time.
- Build: wire the new core module + test into CMake. Persist `show_planner_window` in UI prefs.

### r51: Fog-of-war friendly auto-explore overhaul (survey-first + frontier scoring)

- Core AI: reworks **Auto-explore** so it no longer "peeks" through unsurveyed jump points:
  - Treats unsurveyed jump points as **unknown exits** and first issues a **MoveToPoint** to survey.
  - Prefers transiting **surveyed** exits that are known to lead to **undiscovered systems**.
  - If the current system has no exploration work, routes to a **frontier system** (unknown exits or known exits to undiscovered systems),
    scoring by **frontier work** vs **ETA**.
  - Adds per-faction **reservations** so multiple idle explorers spread across exits/frontiers in the same AI tick.

- Tests: add `test_auto_explore` to lock in the new behaviour (survey-first + jump only when surveyed).
- Build: wire the new test into CMake.

### r50: Colony production forecast (construction + shipyard ETA timeline)

- Core: add `colony_schedule` which simulates a simplified day-by-day economy loop for a single colony and
  estimates completion dates for:
  - **Shipyard queue** (tons/day, minerals per ton)
  - **Construction queue** (CP/day, mineral costs)
  - **Mining + industry** mineral flows (shared finite deposits on a body)
  - New installations come online **next day** (mirrors tick ordering).
  - Optionally simulates **installation_targets auto-queueing** (construction automation).

- UI: Colony tab now includes a **Production forecast (best-effort)** section:
  - Shows a chronological list of predicted completions as **D+days** and absolute dates.
  - Includes toggles for shipyard/construction/auto-targets plus safety limits (max days/events).
  - Caches results per colony + date to avoid recomputing every frame.

- Tests: add `test_colony_schedule` and fix `test_order_planner` to use `content.designs`.
- Build: wire the new core module + test into CMake.

### r49: Research schedule forecast (ETA dates for active + queue)

- Core: add `research_schedule` to estimate completion days for the current active research project and queue.
  - Mirrors `Simulation::tick_research` day-level behaviour:
    - RP is generated once per day from current colony labs.
    - Tech-based research multipliers affect RP income starting the *next* day.
    - Multiple techs can complete in the same day if sufficient RP is banked.
    - Detects and reports when the queue is blocked by missing prerequisites.
- UI: Research tab now includes a compact **Forecast** table:
  - Shows effective **RP/day** as `base × multiplier`.
  - Lists planned completions as **D+days** and absolute completion dates.
  - Marks the project that was active at forecast start with `[A]`.
- Tests: add `test_research_schedule` covering:
  - prerequisite ordering effects
  - multi-completion with a large RP bank
  - multiplier techs applying from the next day
  - queue-blocked detection
  - active progress carryover
- Build: wires the new core module + test into CMake.

### r48: Ship order mission planner (ETA + fuel preview)

- Core: add `order_planner` to simulate queued ship orders and estimate per-order **ETA** and **fuel** usage (movement, jump transits, waits, orbit durations; truncates on indefinite/combat orders).
- UI: ship orders table now shows **ETA** and **Fuel** columns plus a compact plan summary line.
- Tests: add `test_order_planner` covering move-to-body ETA/fuel math and jump transit system/position updates.
- Build: wire the new core module into CMake.


### r47: Jump point surveys (fog-of-war route knowledge)

- Core / simulation:
  - Adds per-faction **jump point survey state**: `Faction::surveyed_jump_points`.
  - Ships automatically **survey nearby jump points**:
    - All ships survey within docking range.
    - **Surveyor-role ships** can survey from their effective sensor range (respects sensor mode / power policy).
    - Jump transit surveys **both ends** (origin + destination) immediately.
  - Under fog-of-war route restrictions, auto-routing / route planning will only traverse **surveyed** jump links.
  - Mutual-friendly factions share jump-point survey intel when intel-sharing becomes active.

- UI:
  - System map:
    - Unsurveyed jump points are rendered dimmer.
    - Jump point tooltips show **Surveyed: Yes/No** and hide the destination until surveyed.
  - Galaxy map:
    - Jump connections are only drawn for **surveyed** links under fog-of-war.
    - “Unknown exits” hints now include **unsurveyed** exits (as well as surveyed exits leading to undiscovered systems).
    - Route preview cache key now includes event sequence to refresh immediately when survey intel changes.

- Serialization / validation / determinism:
  - Save schema bumped to **v40**.
  - Persists `surveyed_jump_points` and includes it in state digest + validation/repair.

- Tests:
  - Extends serialization coverage for `surveyed_jump_points`.


### r46: Sub-day turns (hour-level tick support)

- Core:
  - Adds hour-level time handling and supports sub-day turn sizes.
  - Updates simulation ticks (ships / combat / economy / installations) to scale correctly with smaller `dt_days`.
- UI:
  - Improves turn/tick handling and displays for sub-day time steps.

### r44: Auto-salvage automation + Wreck directory

- Core AI automation:
  - Adds ship automation: **auto-salvage when idle** (`Ship::auto_salvage`).
  - Auto-salvage behavior:
    - If the ship is carrying minerals, it routes to the **nearest friendly colony** and unloads.
    - Otherwise, it selects a best-known wreck in discovered space, queues a salvage pass,
      and repeats.
    - Prevents multiple automated ships from targeting the same wreck by reserving wreck
      targets already referenced by existing salvage orders.
- UI:
  - Adds **Directory → Wrecks** tab:
    - search + system filter
    - sortable table (total tons, age)
    - quick “Go” button to center the system map on the wreck
  - Adds a ship automation toggle: **Auto-salvage wrecks when idle**.
  - Fixes an unbuildable UI reference to `ui.player_faction_id` in the ship salvage UI.
- Serialization / export / determinism:
  - Persist + export `auto_salvage`.
  - Digest now includes `auto_colonize` and `auto_salvage` (previously omitted).
- Tests:
  - Adds `test_auto_salvage` and extends serialization coverage.


### r43: Turn tick presets (1h / 6h / 12h)

- Adds new turn tick presets and strengthens date/time handling for sub-day turns.


### r42: Missile salvos + point defense + save v37

- Core combat:
  - Adds **missile salvos**: weapons can launch projectiles with time-of-flight (ETA in days).
  - Adds **point defense**: ships with PD stats can intercept incoming missile damage for nearby friendly ships.
  - Ships track a per-ship **missile cooldown** (`missile_cooldown_days`) based on the launching design's reload time.
- Content/Tech pipeline:
  - `ComponentDef` can now define (optional) missile & PD stats:
    - `missile_damage`, `missile_range_mkm`, `missile_speed_mkm_per_day`, `missile_reload_days`
    - `point_defense_damage`, `point_defense_range_mkm`
  - `ShipDesign` now carries derived missile/PD stats aggregated from installed components.
- UI:
  - Ship / design panels now display **Beam weapons**, **Missiles**, and **Point defense** separately.
  - Missile cooldown is shown on ships that have missile weapons.
- Serialization / compatibility:
  - Save schema bumped to **v37** to persist missile salvos + missile cooldown, and to include missile/PD-derived stats in custom designs.
  - Also persists `signature_multiplier` for custom designs (previously lost on save/load).


### r41: Auto-colonize automation + explorer AI expansion

- Core:
  - Adds ship automation: **auto-colonize when idle** (`Ship::auto_colonize`).
  - Auto-colonize selects a best colonization target among discovered systems, avoiding:
    - bodies that already have a colony
    - bodies already targeted by an in-flight ColonizeBody order (per-faction)
  - State export now includes ship automation flags (auto-explore/freight/colonize/refuel/repair).
- AI:
  - Explorer AI research plan now includes **colonization_1**.
  - Explorer AI no longer auto-freights colony-capable freighters; it enables **auto-colonize** instead.
  - Explorer AI will keep at least one colonizer in the shipyard queue when colonization targets exist
    (without starving surveyor production).
- UI:
  - Ship panel: adds **Auto-colonize when idle** toggle for ships with colony modules.
  - Auto-colonize is mutually exclusive with auto-explore and auto-freight.
- Tests:
  - Adds `test_auto_colonize` covering order generation and end-to-end colony creation.

### r40: Population transport (colonists)

- Core:
  - Adds ship-borne **colonists / passengers**: `Ship::colonists_millions`.
  - Adds new ship orders:
    - **LoadColonists(colony_id, millions)** (0 = load max)
    - **UnloadColonists(colony_id, millions)** (0 = unload max)
  - Transfers occur when the ship is within docking range of an **owned** colony.
  - Uses the ship design's `colony_capacity_millions` (colony modules) as passenger capacity.
- UI:
  - Ship details now show embarked colonists for ships with colony modules.
  - Ship↔Colony interactions add **Load/Unload Colonists** controls.
- Serialization / compatibility:
  - Save schema bumped to **v36** to persist embarked colonists and the new order types.
  - Fixes a prior mismatch where `GameState::save_version` and the serialization version constant diverged.
- Tests:
  - Adds `test_population_transport` covering load/unload behavior and capacity limits.

### r39: Colony habitability + habitation infrastructure

- Core:
  - Adds a simple, configurable **habitability** model derived from body temperature + atmosphere
    (uses terraforming targets if set; terraform-complete bodies are treated as fully habitable).
  - Adds optional colony life support via installation stat **habitation_capacity_millions**.
    - If a colony is on a hostile world and is short on habitation capacity, population declines
      proportionally to the shortfall (configurable).
    - If fully supported, growth is reduced (configurable) until the world is terraformed.
  - Colonization seeds "prefab" habitation infrastructure on new colonies (uses `SimConfig::habitation_installation_id`).
- Content:
  - Adds a new starting installation: **Infrastructure (Habitation Domes)**.
  - `colonization_1` now unlocks the `infrastructure` installation.
- UI:
  - Colony tab shows a new **Habitability / Life Support** section with habitability %, required vs provided support, and shortfall warnings.
- Scenarios:
  - Sol: Mars Outpost starts with infrastructure; Alpha Centauri Prime and Barnard b now have environment values.
  - Random scenario: procedural bodies now include a basic atmosphere model; forced homeworld is marked terraform-complete.

## Compatibility notes

- Save schema is unchanged (still v34).

### r38: Shared shipyard repairs + auto-repair automation

- Core:
  - Shipyard repairs are no longer applied per-ship per-yard. Instead, each colony provides a **single repair capacity pool**
    per day: `repair_hp_per_day_per_shipyard * shipyard_count`, shared among all docked damaged ships.
  - Adds optional **repair mineral costs** (per HP repaired): `repair_duranium_per_hp` and `repair_neutronium_per_hp`.
    (Defaults to 0 = free repairs.)
  - Adds per-ship **repair priority** (Low/Normal/High) to control which ships get repaired first when capacity is limited.
  - Adds ship automation: **auto-repair when damaged (idle)** with a configurable HP threshold.
- UI:
  - Ship panel: new auto-repair toggle + threshold slider, plus a repair priority dropdown.
- State validation:
  - Clamps auto-refuel/auto-repair thresholds to [0,1] and sanitizes invalid persisted repair priority values.
- Serialization:
  - Save schema bumped to **v34** (older saves still load; new fields default safely).

### r37: Economy window + mineral reserves + tech tree tier view

- UI:
  - Adds a new **Economy** window (View → Economy) with three tabs:
    - **Industry**: colony-by-colony overview of population, construction points/day, research points/day,
      mines, shipyards, queues, and key stockpiles.
    - **Mining**: body-centric mining overview with predicted daily extraction (based on current deposits and installed mines),
      depletion ETA, and per-colony mining breakdown.
    - **Tech Tree**: tiered tech tree view (prereq depth columns) with search, tooltips, and a details pane with
      quick actions (set active, queue, queue prereq plan).
- Core:
  - Adds per-colony **manual mineral reserves** (`Colony::mineral_reserves`):
    - Auto-freight will not export minerals below your configured reserve (in tons).
    - Effective reserve is `max(manual reserve, local queue reserve)` so local shipyard/construction needs stay protected.
  - Save schema bumped to **v25** (older saves still load).
- Tests:
  - Extends auto-freight + serialization tests to cover mineral reserves.

### r36: Colonies & planets directory + UI theme/background options

- UI:
  - Adds a **Directory** window (toggle in View menu) with sortable tables:
    - **Colonies**: filter by faction/system + search, sort by pop/CP/fuel/shipyards.
    - **Bodies**: filter by system/type + search, show deposits total, colonization status + population.
  - Adds a new **Body** tab in the Details panel to inspect the selected planet/body:
    - body type, orbit, position,
    - colony (if present),
    - mineral deposits table.
  - System map improvements:
    - highlights colonized bodies and your currently selected body,
    - **right-click** selects ships or bodies without issuing orders.
- UI customization:
  - Adds a new top menu structure:
    - **View**: toggle windows + reset layout.
    - **Options → Theme**: change clear color + galaxy/system map backgrounds + optional ImGui window background override.
    - **Options → UI Prefs**: load/save UI preferences to a JSON file, plus optional autosave on exit.
  - SDL renderer clear color is now driven by UI settings.

### r35: Ship power budgeting + load shedding + UI/exports

- Core:
  - Adds a prototype **power** model to ship designs:
    - Reactors contribute `power_output`.
    - Components can draw `power_use` (optional; defaults to 0 for legacy content).
  - Ship subsystems are **load-shed** deterministically when power is insufficient, in priority order:
    - engines → shields → weapons → sensors.
  - Sensors/weapon/shield behavior is now gated by available power:
    - unpowered sensors do not contribute to detection,
    - unpowered weapons cannot fire,
    - unpowered shields collapse (set to 0) and stop regenerating.
- Content:
  - Extends `starting_blueprints.json` with `power_use` for advanced components (ion engines, improved lasers, advanced sensors, shields).
- UI:
  - Ship and Design panels now show **Power generation/use** and indicate which subsystems are online.
- Serialization:
  - Save schema bumped to `24` to include new derived power fields in stored custom designs.
- CLI/Export:
  - Ship JSON export now includes fuel and power budget fields (plus online/offline flags).
- Tests:
  - Adds `test_power_system` to assert that weapons are prevented from firing when shed due to power.

### r33: Finite mineral deposits (body) + mining depletion + exports

- Core:
  - Adds `Body::mineral_deposits` (tons remaining per mineral) to model finite resources.
  - Installations can now be flagged as mining extractors (`InstallationDef::mining`).
    - Mining production pulls from body deposits and depletes them.
    - When a deposit hits zero, a persistent `Construction` warning event is emitted.
- Scenarios:
  - Sol and random scenarios now seed basic Duranium/Neutronium deposits on planets.
- UI:
  - Colony panel shows underlying body deposits and (when mining) an estimated depletion ETA.
- CLI / Export:
  - Adds `--export-bodies-json PATH` to export bodies + deposits.
  - `--list-bodies` now includes a deposit total column when deposits are present.
  - Colony JSON export now includes `body_mineral_deposits` and a `mineral_depletion_eta_days` hint.
- Content:
  - Marks `automated_mine` as `"mining": true` in `starting_blueprints.json` for clarity.
- Serialization:
  - Save version bumped to `22`.
  - Saves now include optional per-body `mineral_deposits`.
- Tests:
  - Adds `test_mineral_deposits`.

### r32: Research planner + tech tree export + UI tech browser

- Core:
  - Adds `compute_research_plan()` to compute a prerequisite-ordered research plan (prereqs first) for a target tech.
  - Detects and reports:
    - missing tech prerequisites,
    - self-prereqs,
    - prerequisite cycles.
  - Returns a total cost for the plan.
- UI:
  - Research tab now includes a full **tech browser** (search/filter) instead of only listing currently researchable techs.
  - Adds a **plan** preview panel (steps + total cost).
  - Adds buttons:
    - **Queue with prereqs** (auto-adds missing prerequisites in the correct order),
    - **Replace queue with plan**.
- CLI:
  - Adds tech tree exports:
    - `--export-tech-tree-json PATH` (definitions),
    - `--export-tech-tree-dot PATH` (Graphviz DOT).
  - Adds a research planner:
    - `--plan-research FACTION TECH` (human-readable),
    - `--plan-research-json PATH` (machine-readable; `PATH` can be `-` for stdout).
- Utility:
  - Adds `nebula4x::tech_tree_to_json()` and `nebula4x::tech_tree_to_dot()`.
- Tests:
  - Adds `test_research_planner`.

### r31: Fleet system (persistent ship groups) + JSON export

- Core:
  - Introduces a new persisted entity: `Fleet`.
  - Fleets belong to a faction and contain a list of ship ids + a designated leader.
  - Fleets are stored in `GameState::fleets`.
- Serialization:
  - Save version bumped to `13`.
  - Saves now include a new top-level `fleets` array (older saves without it still load).
- Validation:
  - `validate_game_state()` now validates fleet invariants:
    - faction existence,
    - ships exist and belong to the fleet's faction,
    - ships are not duplicated across fleets,
    - leader is a member (when set).
- Simulation:
  - Adds fleet management helpers (`create_fleet`, `add_ship_to_fleet`, `remove_ship_from_fleet`, etc.).
  - Adds bulk order helpers (`issue_fleet_*`) to issue orders to all ships in a fleet.
  - Automatically prunes stale fleet references on load, and keeps fleets consistent when ships are
    destroyed in combat or scrapped.
- CLI:
  - Adds `--export-fleets-json PATH` (`PATH` can be `-` for stdout).
- Utility:
  - Adds `nebula4x::fleets_to_json()` (includes per-fleet ship summaries).
- Tests:
  - Adds `test_fleets`.
  - Extends `test_state_export` with fleet JSON export coverage.

### r30: ships/colonies JSON export + validate newer order types

- CLI:
  - Adds `--export-ships-json PATH` and `--export-colonies-json PATH` (`PATH` can be `-` for stdout).
- Utility:
  - Adds deterministic JSON export helpers: `nebula4x::ships_to_json()` and `nebula4x::colonies_to_json()`.
    - Exports include resolved names (when content is available) and some computed per-day colony rates.
- Validation:
  - `validate_game_state()` now validates `OrbitBody`, `TransferCargoToShip`, and `ScrapShip` orders instead of
    flagging them as unknown.
- Tests:
  - Adds `test_state_export`.
  - Extends `test_state_validation` to cover the newer order types.

### r29: shipyard repairs for docked ships

- Simulation:
  - Ships docked at a friendly colony with at least one `shipyard` installation now automatically repair HP.
  - New `SimConfig::repair_hp_per_day_per_shipyard` (default: `0.5`).
    - Daily repair = `repair_hp_per_day_per_shipyard * shipyard_count`.
  - Logs a `Shipyard` event when a ship becomes fully repaired (to avoid event spam).
- Tests:
  - Adds `test_ship_repairs`.

### r28: colony population growth/decline (simple)

- Simulation:
  - Adds a simple daily population growth/decline model for colonies.
  - New `SimConfig::population_growth_rate_per_year` (fraction per year; default `0.01`).
    - Example: `0.01` ≈ +1%/year.
    - Negative values model population decline.
- Tests:
  - Adds `test_population_growth`.
- Docs:
  - Updates README feature list.

### r27: combat hit events (non-lethal damage logs)

- Simulation:
  - Combat now emits persistent `Combat` events when ships take damage but survive.
  - Adds `SimConfig` thresholds to control when damage events are emitted and when they are elevated to `Warn`:
    - `combat_damage_event_min_abs`
    - `combat_damage_event_min_fraction`
    - `combat_damage_event_warn_remaining_fraction`
- Tests:
  - Adds `test_combat_events`.

### r26: Save diff tooling + fix missing ship orders + scrapping refunds

- Fix (build-breaking): Restores missing `Order` variants (`OrbitBody`, `TransferCargoToShip`, `ScrapShip`) that
  were referenced by simulation/UI/serialization but absent from `orders.h`.
- Simulation:
  - Scrapping a ship at a colony now:
    - returns any carried cargo minerals to the colony stockpile, and
    - refunds a configurable fraction of shipyard mineral build costs (estimated from design mass).
  - New `SimConfig::scrap_refund_fraction` (default: 0.5).
  - Logs a persistent Shipyard event when a ship is scrapped.
- CLI:
  - Adds `--diff-saves A B` to print a deterministic structural diff between two saves.
  - Adds `--diff-saves-json PATH` (optional) to emit a machine-readable JSON diff report (`PATH` can be `-`).
- Utility:
  - Adds `nebula4x::diff_saves_to_text()` and `nebula4x::diff_saves_to_json()`.
- Tests:
  - Adds `test_save_diff`.

### r25: JSON parser BOM tolerance + CRLF-aware error line/col

- JSON:
  - `json::parse()` now ignores a UTF-8 BOM at the start of the document.
  - JSON parse error reporting now treats Windows CRLF as a **single** newline so line/column numbers
    are accurate for files edited on Windows.
- Tests:
  - Adds `test_json_bom` (parses a BOM-prefixed JSON document).
  - Extends `test_json_errors` with a CRLF regression case.

### r24: event summary CSV export (`--events-summary-csv`)

- Utility:
  - Adds `nebula4x::events_summary_to_csv()` producing a single-row CSV summary with a header.
    (Convenient for spreadsheet dashboards alongside `--export-events-csv`.)
- CLI:
  - Adds `--events-summary-csv PATH` (`PATH` can be `-` for stdout) to export the summary for the
    currently filtered event set.
  - The existing stdout-safety rules apply (only one machine-readable stdout export at a time).
- Tests:
  - Extends `test_event_export` to validate summary CSV formatting.

### r23: validate ship order references in `validate_game_state()`

- Core:
  - `validate_game_state()` now checks `ship_orders` for:
    - entries whose ship id does not exist in `ships`,
    - orders in `order_queue` / `repeat_template` that reference missing ids (bodies, jump points, target ships, colonies),
    - negative `WaitDays.days_remaining` values.
- Tests:
  - Extends `test_state_validation` to cover missing-ship ship_orders entries and invalid `MoveToBody` references.

### r22: forward-compatible ship order loading (drops unknown/invalid orders)

- Serialization:
  - Save deserialization now tolerates **unknown or malformed ship orders**.
  - Instead of aborting the entire load, invalid orders are **dropped** (with a warning logged).
- Tests:
  - Extends `test_serialization` with a regression case that injects a fake future order type and
    verifies the save still loads and the unknown order is dropped.

### r21: game state integrity validator + CLI `--validate-save`

- Core:
  - Adds `validate_game_state()` to detect broken saves / inconsistent scenarios (missing ids, mismatched cross-links, non-monotonic `next_id`/`next_event_seq`).
- CLI:
  - Adds `--validate-save` to validate the loaded (or newly created) game state and exit with a non-zero code if issues are found.
- Tests:
  - Adds `test_state_validation` to lock in expected behavior (Sol scenario is valid; injected corruptions are reported).


### r20: JSON parse errors now include line/col + a context caret

- JSON:
  - JSON parse errors now report **line/column** (1-based) in addition to the raw byte offset.
  - Includes a small context snippet of the line with a `^` caret pointing at the failure location.
    (Helps a lot when hand-editing content JSON for modding.)
- Tests:
  - Adds `test_json_errors` to verify line/col + caret formatting for a couple of common failure modes.

### r19: more robust save loading for missing metadata fields

- Serialization:
  - `deserialize_game_from_json()` now treats `save_version`, `next_id`, and `selected_system` as optional,
    making it more tolerant of very early prototypes or hand-edited saves.
  - Invalid/missing `selected_system` is repaired to `kInvalidId`.
- Tests:
  - Extends `test_serialization` to cover deserializing saves with the above keys removed.

### r18: JSON \uXXXX unicode escape support (UTF-8) + tests

- JSON:
  - The minimal JSON parser now fully decodes `\uXXXX` escapes into UTF-8.
  - Correctly handles UTF-16 **surrogate pairs** (e.g. emoji like `\uD83D\uDE80`).
  - Invalid/unpaired surrogate sequences now fail parsing instead of silently corrupting text.
- Tests:
  - Adds `test_json_unicode` to cover BMP codepoints, surrogate pairs, round-tripping, and invalid inputs.

### r17: atomic file writes for saves/exports + file I/O test

- Utility:
  - `write_text_file()` now writes via a **temporary sibling file + rename** to avoid leaving a partially
    written/truncated file behind on crash or interruption (best-effort replace on Windows).
- Tests:
  - Adds `test_file_io` to cover overwrite behavior and ensure temp siblings are not left behind.

### r16: fix CLI stdout-safe status stream

- CLI:
  - Fixes a bug where the CLI tried to choose between `std::cout` and `std::cerr` using an
    uninitialized reference (undefined behavior).
  - Status output now correctly goes to **stderr** when a machine-readable export is sent to stdout
    (`PATH='-'`), and to **stdout** otherwise.

### r15: tech tree cycle detection + stricter tech validation

- Content validation:
  - Detects prerequisite **cycles** in the tech tree (prevents research deadlocks).
  - Flags self-referential prerequisites and empty prereq ids.
  - Adds basic checks for empty tech names and malformed effects (empty type/value).
- Tests:
  - Extends `test_content_validation` to cover cycle detection.

### r14: event summary JSON + stdout-safe mode for exports

- Utility:
  - Adds `nebula4x::events_summary_to_json()` to generate a machine-friendly JSON summary (count, day/date range, counts by level/category).
- CLI:
  - Adds `--events-summary-json PATH` (`PATH` can be `-` for stdout) to export a JSON summary for the filtered event set.
  - When exporting machine-readable output to stdout (`PATH='-'`), non-essential status output is routed to **stderr** automatically and incompatible stdout combinations are rejected.
- Docs + tests:
  - Updates README + `docs/EVENT_LOG.md`.
  - Extends `test_event_export` to cover the summary JSON format.

### r13: event log context filters (system/ship/colony) + targeted time-warp

- Simulation:
  - Extends `EventStopCondition` with optional `system_id`, `ship_id`, and `colony_id` filters.
  - `advance_until_event` now supports stopping on events scoped to a particular system/ship/colony.
- CLI:
  - Adds new event log filters: `--events-system`, `--events-ship`, `--events-colony`.
  - These accept either a numeric id or an exact name (case-insensitive), matching `--events-faction` behavior.
  - The filters apply consistently across `--dump-events`, `--events-summary`, all event exports, and `--until-event`.
- UI:
  - Log tab adds **System / Ship / Colony** filters; Copy/Export actions respect the extended filters.
  - Auto-run (pause on event) adds the same context filters for more precise time-warping.
- Docs:
  - Updates README + `docs/EVENT_LOG.md` with the new filters.

### r12: JSONL/NDJSON event log export + script-safe stdout

- Adds `nebula4x::events_to_jsonl()` (JSON Lines / NDJSON) alongside existing CSV/JSON array exporters.
- CLI:
  - New flag: `--export-events-jsonl PATH` (supports `PATH = '-'` for stdout, like the other exporters).
  - When `--quiet` is used, the `--until-event` status line is now written to **stderr** so stdout can remain clean for machine-readable exports.
  - Guards against accidentally requesting multiple `--export-events-*` outputs to stdout at the same time.
- UI:
  - Log tab adds **Export JSONL**.
  - Export buttons now gently auto-fix the file extension when the export path looks like a common default (events.csv/json/jsonl).
- Docs:
  - Adds `docs/EVENT_LOG.md` documenting event export formats and the schema.
- Tests:
  - Extends `test_event_export` to cover JSONL output and CSV quote escaping.

### r11: UI JSON event export + shared exporter in Log tab

- UI Log tab:
  - Adds **Export JSON** (writes the currently filtered/visible events to a structured JSON array).
  - Refactors the existing **Export CSV** to use the shared `nebula4x::events_to_csv()` helper.
  - Both exports preserve the existing behavior of exporting in chronological order within the visible set.
- CLI help text + README:
  - Documents that `--export-events-csv/-json` accept `PATH = '-'` to write directly to stdout (script-friendly with `--quiet`).
- Tests:
  - Extends `test_event_export` to assert JSON output ends with a trailing newline.

### r10: fix `--export-events-json` + shared event export utils + tests

- Fixes a CLI bug where `--export-events-json PATH` only worked when `--export-events-csv` was also provided.
- Adds `nebula4x::events_to_csv()` and `nebula4x::events_to_json()` (shared helpers) under `nebula4x/util/event_export.*`.
  - The CLI now uses these helpers for both exports.
- Adds a small unit test (`test_event_export`) to prevent regressions.

### r9: CLI list bodies/jumps + export events JSON

- Adds two more script-friendly CLI helpers:
  - `--list-bodies` prints body ids/names plus basic context (type/system/orbit/position).
  - `--list-jumps` prints jump point ids/names plus system + linked destination.
- Adds `--export-events-json PATH` to export the persistent simulation event log to structured JSON.
  - Respects the same `--events-*` filters as `--dump-events` / `--export-events-csv`.
- Updates README examples to include the new helpers/JSON export.

### r8: CLI list ships/colonies + macOS CI + gitignore QoL

- Adds two more script-friendly CLI helpers:
  - `--list-ships` prints ship ids/names plus basic context (faction/system/design, hp, cargo, order queue length).
  - `--list-colonies` prints colony ids/names plus basic context (faction/system/body, population, queue sizes).
- Expands GitHub Actions CI to include **macos-latest** (core + CLI + tests; UI still OFF).
- Extends `.gitignore` with common CMake and editor artifacts (`compile_commands.json`, `.vscode/`, `cmake-build-*`, etc.).

### r7: CI + gitignore + CLI list helpers

- Adds a GitHub Actions workflow at `.github/workflows/ci.yml`.
  - Builds **core + CLI + tests** with `NEBULA4X_BUILD_UI=OFF` (no SDL2/ImGui dependency in CI).
  - Runs unit tests via `ctest` on **ubuntu-latest** and **windows-latest**.
  - Uses a Release build type/config for consistency.
- Adds a `.gitignore` for common CMake build output (`build/`, `out/`) and editor artifacts.
- Adds two small CLI quality-of-life flags:
  - `--list-factions` prints faction ids + names then exits.
  - `--list-systems` prints system ids + names (with basic counts) then exits.

### r6: CLI event log date range + summary

- Adds `--events-since X` and `--events-until X` filters for the persistent event log.
  - `X` can be either a **day number** (days since epoch) or an ISO date `YYYY-MM-DD`.
- Adds `--events-summary` to print counts by level/category for the filtered event set.
- Makes `--dump-events` / `--events-summary` / `--until-event` output avoid a leading blank line in `--quiet` mode (more script-friendly).

### r5: build fixes (MSVC)

- Fixes MSVC warning C4456 in `serialization.cpp` by avoiding variable shadowing in an `if/else if` chain.
- Fixes MSVC warning C4456 in `panels.cpp` by renaming a nested `factions` variable.
- Fixes a build break in the Design panel (`cargo_used_tons` undeclared) by defining it (designs don't carry cargo).

### CLI: time warp until a matching event (`--until-event N`)

Adds a new CLI option:

- `--until-event N`

This advances the simulation **day-by-day** up to `N` days, stopping early when a **newly recorded** persistent simulation event matches the stop criteria.

Notes:

- When `--until-event` is used, `--days` is ignored.
- Stop criteria is defined via the existing `--events-*` flags:
  - `--events-level` (defaults to `warn,error` for `--until-event` unless explicitly provided)
  - `--events-category`
  - `--events-faction`
  - `--events-contains`
- On completion, the CLI prints a single **hit / no-hit** status line (even in `--quiet` mode) so the feature is script-friendly.

### Docs

- README updated with event log analysis examples and the new `--list-*` CLI helpers.

## Compatibility notes

- **Save schema is unchanged** (still v12).
- CI and `.gitignore` changes do not affect runtime behavior.

### r43: sub-day turn ticks (1h / 6h / 12h)

- Adds `Simulation::advance_hours()` and introduces an `hour_of_day` field in `GameState`.
- UI: adds quick turn buttons `+1h`, `+6h`, `+12h` (status bar + sidebar) and displays `YYYY-MM-DD HH:00`.
- Simulation: splits each turn at midnight and runs daily economy only once per day, while running
  continuous systems (ship movement, contacts, shields, combat) on sub-day ticks.
- Combat scaling: beam damage, shields regen, and missile ETA/cooldowns now scale by elapsed time.
- Order timing: `WaitDays`, `OrbitBody`, and `BombardColony` use a `progress_days` accumulator so they
  behave consistently under sub-day ticks.
- Boarding now has a per-ship daily cooldown to avoid multiple boarding attempts in one day.

Compatibility:
- Save schema bumped to v38; older saves load with `hour_of_day = 0`.

### r45: Event timestamps with hour-of-day + hour-resolution time warp + timeline zoom

- **SimEvent now records `hour`** (0..23) in addition to `day`, enabling accurate mid-day logging when using sub-day ticks.
- **UI: Event log and timeline** now display `YYYY-MM-DD HH:00` and the timeline plots events at sub-day positions.
- **Timeline zoom**: increased max zoom so you can inspect hour-level activity, with sub-day grid ticks/labels when zoomed in.
- **Event exports** (`--export-events-*`): added `hour`, `time`, and `datetime` fields to JSON/JSONL and appended `hour,time,datetime` columns to CSV (kept at end for backward-ish compatibility).
- **Time warp until event**: added `Simulation::advance_until_event_hours(max_hours, stop, step_hours)` and UI auto-run now lets you choose an event-check step of **1h / 6h / 12h / 1d**.

Compatibility:
- Save schema bumped to v39 (adds optional `hour` field to serialized events; older saves load with `hour = 0`).

### r46: Auto-tanker logistics (ship-to-ship refueling) + test cleanup

- **New ship automation: Auto-tanker**
  - When enabled, an **idle** ship with fuel capacity will act as a fuel tanker.
  - It automatically routes to **friendly idle ships** that are **low on fuel** (and have **auto-refuel disabled**) and performs **ship-to-ship fuel transfer**.
  - Tankers keep a configurable **reserve fraction** of their own fuel capacity and will not transfer below it.
- **Simulation config:** adds a few knobs in `SimConfig`:
  - `auto_tanker_request_threshold_fraction`
  - `auto_tanker_fill_target_fraction`
  - `auto_tanker_min_transfer_tons`
- **UI:** ship Automation panel adds an Auto-tanker toggle + reserve slider.
- **State plumbing:** serialization, validation, digest, and exports updated for the new ship fields.
- **Tests:** fixes a broken `test_auto_salvage` and adds `test_auto_tanker`.

Compatibility:
- **Save schema is unchanged** (still v39). New fields default to disabled on older saves.

### r47: UI Layout Profiles (multiple dock layouts) + workspace presets

- **New UI tool window: Layout Profiles**
  - Save/reload the current Dear ImGui docking layout to a named profile.
  - Switch between profiles at runtime (loads the profile's ini before `NewFrame` to keep docking stable).
  - Duplicate, rename, and delete profile files.
  - Uses `ui_layouts/<profile>.ini` (directory auto-created on demand).
- **Workspace presets** (Default / Minimal / Economy / Design / Intel)
  - Quick one-click set of window visibility to match a task.
  - Available both in the Layout Profiles window and via the command palette.
- **Shortcuts & menus**
  - **Ctrl+Shift+L** toggles the Layout Profiles window.
  - Added to **Tools** menu and the command palette.

Compatibility:
- **No save schema changes.** UI prefs schema bumped to v16 (adds layout profile fields).

### r48: Procedural JSON Explorer (State Inspector) + JSON Pointer utilities

- **New Tools window: JSON Explorer** (procedurally generated tree/table inspector)
  - Inspect the **current in-memory game state** (serialize → parse) with manual **Refresh** and optional **auto-refresh**.
  - Load and browse **arbitrary JSON files** and your **autosaves**.
  - Data-driven UI:
    - Tree view with **search filtering**, **match highlighting**, and **breadcrumb navigation**.
    - **Go to JSON Pointer** (RFC 6901) to jump directly to any node and auto-open its ancestors.
    - Array **table view**: for arrays of objects, auto-discovers keys and renders a sortable table so you can scan large structures quickly.
- **New util module:** `json_pointer.h/.cpp`
  - Centralized JSON Pointer helpers: escape/unescape, split, join, and resolve.
  - Adds `test_json_pointer` for basic coverage.

Compatibility:
- **No save schema changes.** UI prefs schema bumped to v17 (adds `show_json_explorer_window`).

### r49: Colony founding defaults (auto-apply automation profile on colonization)

- **New faction-level "Colony founding defaults" profile**
  - Installation targets (auto-build), mineral reserves (export floor), mineral targets (import goal), and garrison target.
  - When enabled, this profile is **auto-applied** to **newly established colonies** created via **ColonizeBody** (manual or AI).
- **UI: Colony Profiles window**
  - Adds a new **"Founding Defaults"** tab with a full editor for the founding profile.
  - Quick workflow: load from a saved profile, capture from a colony, save as a reusable profile.
  - Optional helpers to apply the founding defaults to existing colonies (selected / all).
- **Simulation**
  - Colonization now optionally applies the founding defaults and annotates the "Colony established" event with the applied profile label.

Compatibility:
- **Save schema is unchanged** (still v42). New faction fields default to disabled on older saves.
