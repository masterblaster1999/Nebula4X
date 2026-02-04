## Round 51

- **Delta-save conversion:** added `convert_delta_save_patch_kind()` to convert a delta-save between merge_patch (RFC 7396) and json_patch (RFC 6902) while preserving patch count.
  - CLI: `--convert-delta-save DELTA` (select output kind via `--delta-save-jsonpatch`)
  - CLI: `--delta-save-stats DELTA` prints patch size stats (min/med/max/avg)

Verification:
- Unit: `tests/test_save_delta.cpp`

---

## Round 50

- **CI:** added GitHub Actions workflow to build + run tests headless (no UI) on Ubuntu and Windows.
- **Delta-save tooling:** added `squash_delta_save_as()` / `squash_delta_save()` to compact history to a single patch (or snapshot).
  - CLI: `--squash-delta-save DELTA` (optionally rebase via `--delta-save-index N`, optionally convert via `--delta-save-jsonpatch`).
- **Delta-save performance:** `compute_delta_save_digests()` and CLI `--verify-delta-save` now reconstruct incrementally (O(n) instead of O(n^2)).

Verification:
- Unit: `tests/test_save_delta.cpp`

---

## Round 49

- **Time Machine (smarter delta history):** added a new storage mode **Delta (JSON Patch)** alongside existing **Delta (Merge Patch)**.
  - Mode 2 stores RFC 6902 JSON Patch operations between snapshots (often smaller/more precise for arrays).
  - Delta-save export now matches the active delta storage type (merge patch v1 vs JSON patch v2).
- **Delta-save v2:** delta-save files now support a `patch_kind` field and JSON Patch patch chains.
  - CLI: `--delta-save-jsonpatch` to create v2 delta-save files.
- **JSON Patch apply:** exposed a `apply_json_patch(json::Value&, json::Value)` overload to apply patches without repeated parse/stringify.

Verification:
- Unit: `tests/test_save_delta.cpp`

---

## Round 48

- **Save merge (even smarter arrays):**
  - Added *insertion-wise* array merging for append/insert-only changes when the base array is a subsequence of both LOCAL and REMOTE.
    - Reduces conflicts for log-like or set-like arrays of primitives without requiring a merge key.
    - Option: `merge_arrays_by_insertions` (default: on), guardrail `max_array_insertion_merge_elems`.
  - Added *auto key discovery* for key-wise array merging when `id/guid/uuid` aren’t present.
    - Option: `auto_discover_array_key` (default: on).
- **CLI:** extended `--merge-saves` with:
  - `--merge-saves-no-insertion-merge`
  - `--merge-saves-no-auto-key-discovery`

Verification:
- Unit: `tests/test_save_merge.cpp`

---

## Round 47

- **Save merge (smart arrays):** three-way merge now supports *key-wise* merging for arrays of objects with a unique id-like field.
  - Greatly reduces conflicts for common save patterns like `[{"id":...}, ...]` when one side adds/removes elements and the other side edits fields.
  - Options: `merge_arrays_by_key`, `array_key_candidates` (default: id,guid,uuid), `array_key_override`, `max_array_key_merge_elems`.
- **CLI:** extended `--merge-saves` with:
  - `--merge-saves-no-key-merge` to disable key-wise array merging.
  - `--merge-saves-array-key KEY` to force a specific key name.

Verification:
- Unit: `tests/test_save_merge.cpp`
- CLI: `--merge-saves BASE.json LOCAL.json REMOTE.json --merge-saves-report -` (try with arrays of `{id:...}` objects)

---

## Round 46

- **Save tooling:** added a three-way save merge utility (BASE + LOCAL + REMOTE) with a structured conflict report and optional auto-resolution strategy.
  - New CLI command: `--merge-saves BASE LOCAL REMOTE` (+ `--merge-saves-out`, `--merge-saves-report`, `--merge-saves-strategy`, `--merge-saves-no-array-merge`).
  - New util module: `save_merge.h/.cpp`.
- **Docs/Comments:** corrected JSON Merge Patch RFC number to **RFC 7396** across CLI/UI/docs.

Verification:
- Unit: `tests/test_save_merge.cpp`
- CLI: run `--merge-saves` with a known base + two variants; inspect report output on conflict.

---

## Round 45

- **CLI (Save diff JSON Patch):** added `--diff-saves-jsonpatch-tests` to emit RFC 6902 JSON Patch output with `test` preconditions (safer when applying to drifted bases).

Verification:
- CLI: `--diff-saves A.json B.json --diff-saves-jsonpatch OUT.patch.json --diff-saves-jsonpatch-tests`
- Unit: `tests/test_save_diff.cpp`

---

## Round 44

- **JSON Patch generation (RFC 6902):** `emit_tests` now also emits `test` preconditions for `move`/`copy` sources (in addition to `replace`/`remove`) so drifted bases fail fast.
- **Docs/Tests:** updated `save_diff.h` docs and added coverage for `test`+`move` and `test`+`copy` (including array append-copy via `/-`).

Verification:
- Unit: `tests/test_save_diff.cpp`

---

## Round 43

- **JSON Patch generation (RFC 6902):** object-level duplicate-value replacements/additions can now emit `copy` from keys that become stable earlier in the same object diff (not just keys that were already identical).
- **Headers/Docs:** restored the `include/nebula4x/util/*.h` patch set in the cumulative patch zip and updated `save_diff.h` docs for `emit_tests`.
- **Tests:** added coverage for copying from an earlier-patched key within the same object diff.

Verification:
- Unit: `tests/test_save_diff.cpp`

---

## Round 42

- **JSON Patch generation (RFC 6902):** added optional `JsonPatchOptions::emit_tests` to emit `test` preconditions before `replace`/`remove` operations (safer application on drifting bases, at the cost of larger patches).
- **Tests:** added coverage that verifies `test`+`replace` emission and that mismatched bases fail fast.

Verification:
- Unit: `tests/test_save_diff.cpp`

---

## Round 41

- **JSON Patch generation (RFC 6902):** when an object key’s value changes to a value that already exists on another *stable* key within the same object, emit a `copy` op (instead of nested diffs/replaces).
- **Safety:** `copy` emission now uses strict JSON equality to avoid copying "almost equal" numeric values.
- **Docs/Tests:** updated `save_diff.h` docs and added coverage in `tests/test_save_diff.cpp`.

Verification:
- Unit: `tests/test_save_diff.cpp`

---

## Round 40

- **JSON Patch generation (RFC 6902):** when inserting/appending array elements whose value already exists at a stable earlier index (including earlier inserted values), emit a `copy` op instead of embedding the full value (smaller patches for repeated subtrees).
- **Docs/Tests:** documented the array `copy` heuristic in `save_diff.h` and added coverage in `tests/test_save_diff.cpp`.

Verification:
- Unit: `tests/test_save_diff.cpp`

---

## Round 39

- **JSON Patch generation (RFC 6902):** when adding an object key whose value already exists on another stable key within the same object, emit a `copy` op instead of embedding the full value (keeps patches smaller for repeated subtrees).
- **Docs/Tests:** updated `save_diff.h` docs and added a generator test for the new `copy` emission.

Verification:
- Unit: `tests/test_save_diff.cpp`

---

## Round 38

- **JSON Patch generation (RFC 6902):** avoid emitting `move` + full-element `replace` for scalar/type-changes during array relocation detection (falls back to clearer `remove` + `add` shift/insert heuristics instead).
- **Tests:** existing shift-left test in `tests/test_save_diff.cpp` now passes with the intended `remove` + `add` output.

Verification:
- Unit: `tests/test_save_diff.cpp`

---

## Round 37

- **JSON Patch generation (RFC 6902):** prefer `"/-"` for array appends when emitting `add` operations at the end of an array.
  - Applies to both simple append cases and LCS-aligned array diffs.

- **Tests:** added coverage for `add` appends using `"/arr/-"` (including insertion+append sequences).

Verification:
- Unit: `tests/test_save_diff.cpp`

---

## Round 36

- **JSON Patch generation (RFC 6902):** when an array element is relocated *and* edited, emit `move` + a nested diff at the destination (keeps patches small for reorder+edit workflows).
- **JSON Patch generation (RFC 6902):** conservatively detect **rename + edit** when exactly one object key is removed and one is added with container values of the same type; emit `move` + nested diffs instead of remove+add of large objects.
- **Tests:** added coverage for both scenarios.

Verification:
- Unit: `tests/test_save_diff.cpp`

---

## Round 35

- **JSON Patch generation (RFC 6902):** added an LCS-based alignment step for small arrays when the trimmed middle windows differ in length. This prevents noisy replace cascades when items are inserted/removed and later elements also change.
- **Tests:** added coverage for insertion + later modification producing a concise `add` + `replace`.

Verification:
- Unit: `tests/test_save_diff.cpp`

---

## Round 34

- **JSON Patch apply (RFC 6902):** improved error diagnostics by including the failing operation index and basic context (`op`, `path`, optional `from`).
- **Tests:** added coverage ensuring both apply-time failures and parse-time errors include op indices.

Verification:
- Unit: `tests/test_save_diff.cpp`

---

## Round 33

- **JSON Patch generation (RFC 6902):** detect simple object member *renames* (removed key + added key with identical value) and emit a single `move` op instead of `remove`+`add`.
- **Tests:** added coverage for object rename-as-move.

Verification:
- Unit: `tests/test_save_diff.cpp`

---

## Round 32

- **JSON Patch generation (RFC 6902):** detect a single relocated element within the trimmed array window and emit a single `move` op (not just rotations).
- **Tests:** added coverage for swap-like moves and moving a tail element into the middle.

Verification:
- Unit: `tests/test_save_diff.cpp`

---

## Round 31

- **JSON Patch generation (RFC 6902):** detect simple array one-step shifts/rotations to emit smaller patches.
  - Shift + new element uses `remove` + `add` instead of cascades of `replace`.
  - Pure rotations use a single `move`.
- **JSON Patch JSON output:** includes `"from"` for `move`/`copy` ops.
- **Tests:** added coverage for shift and rotation cases.

Verification:
- Unit: `tests/test_save_diff.cpp`

---

## Round 30

- **JSON Patch generation (RFC 6902):** improved array diffing by trimming common prefix/suffix before generating operations. This greatly reduces noisy patches when items are inserted/removed near the front or middle of arrays.
- **Tests:** added coverage ensuring simple array insertions/deletions produce concise `add`/`remove` operations.

Verification:
- Unit: `tests/test_save_diff.cpp`

---

## Round 29

- **JSON Patch apply (RFC 6902):** implemented `move`, `copy`, and `test` operations.
  - `test` uses strict logical JSON equality (object member ordering is ignored).
  - `move` enforces the RFC rule that `from` must not be a proper prefix of `path`.
- **Tests:** added coverage for `move`, `copy`, and `test`.

Verification:
- Unit: `tests/test_save_diff.cpp`

---

## Round 28

- **JSON Patch apply (RFC 6902):** aligned array-index parsing with Nebula4X JSON Pointer rules.
  - Leading zeros are rejected (except the single token `"0"`).
  - Indices are parsed into `std::size_t` so out-of-range values fail cleanly (no truncation).
- **Tests:** added coverage for leading-zero array indices in JSON Patch apply.

Verification:
- Unit: `tests/test_save_diff.cpp`

---

## Round 27

- **JSON Pointer index parsing:** tightened array-index token rules and improved safety.
  - Leading zeros are rejected (except the single token `"0"`) to match UI/autocomplete expectations.
  - Indices are parsed directly into `std::size_t` so out-of-range values fail instead of truncating.
- **JSON Pointer glob queries:** non-numeric array segments are treated as "no match" (not a fatal error), so patterns like `/**/k` work even in docs that contain arrays.
- **Tests:** added coverage for leading-zero and out-of-range array index tokens.

Verification:
- Unit: `tests/test_json_pointer.cpp`, `tests/test_json_pointer_glob.cpp`

---

## Round 26

- **JSON Pointer autocomplete:** improved array-index completion for large arrays.
  - Numeric prefixes (e.g. `/big/99`) now generate matching indices across the full array without a fixed scan window.

Verification:
- `nebula4x_cli --complete-json-pointer save.json "/systems/0/n"`
- `echo '{"big":[]}' | nebula4x_cli --complete-json-pointer - "/big/"`
- Unit: `tests/test_json_pointer_autocomplete.cpp` (large array case)

---

## Round 25

- **CLI (`--complete-json-pointer`):** added a JSON Pointer autocomplete mode for scripts and debugging.
  - `--complete-json-pointer FILE PREFIX` prints full-pointer suggestions (FILE can be `-` for stdin; PREFIX may omit leading `/`).
  - Tuning flags:
    - `--complete-max N`
    - `--complete-case-sensitive`
- **CLI (`--query-json`):** PATTERN is now forgiving if you omit the leading `/` (it will be added automatically).

Verification:
- `nebula4x_cli --complete-json-pointer save.json "/systems/0/n"`
- `echo '{"a":{"b":1}}' | nebula4x_cli --complete-json-pointer - "/a/"`
- `nebula4x_cli --query-json save.json "**/name"`

---

## Round 24

- **CLI (`--query-json`):** `FILE` can now be `-` to read the JSON document from **stdin**.
- **CLI (`--query-json`):** added `--query-json-jsonl PATH` to emit matches as **JSONL/NDJSON** (`PATH` can be `-` for stdout).
- **CLI (events):** `--events-category` now supports `terraforming` and help text includes `diplomacy|terraforming`.

Verification:
- `echo '{"a":1}' | nebula4x_cli --query-json - "/a"`
- `nebula4x_cli --query-json save.json "/**/name" --query-json-jsonl -`
- `nebula4x_cli --dump-events --events-category terraforming`

---

## Round 23

### Added: `--query-json` CLI utility (JSON pointer glob queries)

A lightweight JSON inspection mode for scripts and debugging:

- `--query-json FILE PATTERN` prints matching JSON pointer paths (and compact value snippets) to stdout.
- `--query-json-out PATH` writes a machine-readable JSON report containing matches + traversal stats.
- Tuning flags:
  - `--query-max-matches N`
  - `--query-max-nodes N`
  - `--query-max-value-chars N`

### Improved: JSON pointer glob matching

- Deterministic traversal: when expanding objects, keys are visited in sorted order.
- Added per-segment glob patterns:
  - `*` matches any substring (including empty)
  - `?` matches exactly one character
  - Backslash escapes `*`, `?`, and `\` for literal matches

Verification:
- `tests/test_json_pointer_glob.cpp` covers `*`/`**`, segment glob patterns, and escaping.
- Example:
  - `nebula4x_cli --query-json my.json "/e/1*"`

---

## Round 22

### Added: Retarget macro “Auto-map by system” (UI)

When you copy a route that references **many** entities in one system (bodies / colonies / jump points), manually filling the mapping tables can still be tedious.
This round adds an **Auto-map by system (name match)** helper inside **Ship panel → Orders → Retarget selection**.

**How it works**
- Pick a **From** system (auto-detected from the selected orders) and a **To** system (defaults to the ship’s current system).
- Click **Auto-map by name**:
  - Attempts to fill mapping entries by **matching normalized names** in the destination system.
  - Bodies disambiguate by **BodyType** when multiple candidates share the same normalized name.
  - Colonies prefer mapping via the mapped **body** first, then fall back to colony name matching.
  - Jump points attempt to disambiguate by linked-destination system name (when visible).
- Produces an **auto-map report** (copyable) showing mapped/missing/ambiguous/skipped counts by type.
- Fog-of-war safe: the macro only indexes **visible** entities under current discovery/detection rules.
- The macro only *fills the mapping*; nothing is changed until you click **Apply mapping to selection**.

Verification:
- In **Ship panel → Orders**, select a set of orders that reference multiple bodies/colonies/jump points.
- Open **Retarget selection → Auto-map by system (name match)**.
- Pick From/To systems and click **Auto-map by name**:
  - Confirm mapping tables are pre-filled.
  - Confirm the report shows reasonable mapped/missing counts.
- Click **Apply mapping to selection** and confirm the selected orders update.
- Press **Undo** to confirm the change is undoable.

---

## Round 21

### Added: Order-queue “Retarget selection” reference mapper (UI)

**Why:** Copying a mining/logistics route and then changing its destination (or source) used to require manually editing each order.  
Now you can select any subset of queued orders and remap their referenced IDs (bodies / colonies / jump points / ships / anomalies / wrecks / systems) in one place.

**Highlights**
- Scans the selected orders for common ID fields (`body_id`, `colony_id`, `dropoff_colony_id`, `jump_point_id`, `target_ship_id`, `anomaly_id`, `wreck_id`, `system_id`, `last_known_system_id`).
- Presents a per-type mapping table: **From → To**, with an occurrence count per reference.
- Fog-of-war safe: candidate lists are filtered using the same visibility rules as portable template import (discovered systems, surveyed jump points, detected ships, discovered anomalies).
- Optional per-type filter text boxes to keep dropdowns usable in large saves.
- Fully undoable (integrates with the queue editor undo/redo stack).

---

# Round 20 Notes

- **UI (Ship Orders queue editor):** added power-user **keyboard shortcuts**:
  - **Ctrl+C / Ctrl+X / Ctrl+V**: copy / cut / paste selection (portable JSON)
  - **Delete**: delete selection
  - **Ctrl+D**: duplicate selection
  - **Ctrl+A**: select all orders in the queue
  - **Esc**: clear selection
  - **Ctrl+Z / Ctrl+Y** (or **Ctrl+Shift+Z**): undo / redo

- **UI (Smart rebuild route):** added a **Smart rebuild route** button (and hotkey **Ctrl+Shift+R**) to recompile the *entire* queue with smart routing.
  - Optionally **strip TravelViaJump** first (default on) so the router rebuilds travel legs from scratch.
  - Optionally **restrict to discovered systems** when fog-of-war is enabled.

- **Core (Smart routing quality):** `compile_orders_smart(...)` now starts from a better predicted navigation state when appending orders, using a new helper:
  - `sim_nav::predicted_nav_state_after_queued_orders(...)` (best-effort; simulates deterministic position changes + cross-system travel)

Verification:
- In **Ship panel → Orders**:
  - Use Ctrl+A then Ctrl+C; paste into a text editor and confirm portable JSON.
  - Use Ctrl+Z / Ctrl+Y after delete/duplicate/paste and confirm the queue rewinds correctly.
  - Click **Smart rebuild route** on an edited queue and confirm TravelViaJump legs are rebuilt.

---

# Round 19 Notes

- **UI (Ship Orders queue editor):** the ship order queue is now a **proper editor** with multi-selection + clipboard workflows.
  - Click to select a row, **Ctrl+Click** toggles, **Shift+Click** range selects.
  - New buttons:
    - **Copy sel** (portable JSON) – copies the selected orders as a portable template JSON.
    - **Cut** – copies and removes selected orders.
    - **Del sel** – deletes selected orders.
    - **Dup sel** – duplicates selected orders (new copies become selected).
    - **Paste** – inserts orders from clipboard (portable or legacy JSON accepted).
  - Paste supports **Insert at** modes (start/end/before/after selection) and an optional **Replace selection** toggle.

- **UI (Paste: resolve references modal):** when pasted portable JSON contains **ambiguous references**, an in-place modal appears to resolve them before insertion:
  - **Auto-pick first candidates** helper
  - **Copy resolution report** for debugging/shareability
  - **Finalize paste** becomes available once all refs are resolved

- **UI (Undo/Redo):** queue edits now have per-ship **Undo** / **Redo** (history depth 32).  
  - Works for bulk operations (cut/delete/duplicate/paste) and single-row edits (dup/del/move).

- **Core (Simulation):** added `Simulation::set_queued_orders(...)` as a safe bulk queue replacement helper (clears emergency suspension; does not touch repeat settings).

Verification:
- In **Ship panel → Orders**:
  - Select multiple orders (ctrl/shift) → **Copy sel**, paste into a text editor, confirm JSON.
  - **Cut** a selection → verify the queue shrinks and clipboard has JSON.
  - **Undo** brings the removed orders back; **Redo** removes them again.
  - Paste a portable template that needs resolution → verify the **Resolve references** modal appears and **Finalize paste** inserts orders.

---

# Round 18 Notes

- **UI (Portable template import: resolution wizard):** portable clipboard templates can now be imported even when some references are **ambiguous**.
  - **Paste template JSON from clipboard** now performs a *partial* import and, if needed, shows a **Resolve references (required)** table.
  - Each ambiguous `*_ref` entry becomes a dropdown with candidate matches from the current save (fog-of-war safe).
  - Once all issues are resolved, click **Finalize import** to convert the template into real orders (enabling preview/apply/save).
  - Added **Copy resolution report** to clipboard for debugging/shareable diagnostics when a template can't be fully resolved.

- **UI (Portable import API):** added an interactive import session API in `ui/order_template_portable.*`:
  - `start_portable_template_import_session(...)` parses + auto-resolves what it can and returns a list of unresolved issues.
  - `finalize_portable_template_import_session(...)` applies the chosen resolutions and runs the canonical parser.
  - Matching now includes a small **fuzzy normalization** (alphanumeric, case-insensitive) to tolerate minor naming differences.

Verification:
- Copy a portable template JSON between two saves that share similarly-named entities.
- If a name matches multiple entities, the import should stop and show a **Resolve references** table.
- Select matches, click **Finalize import**, then verify:
  - Imported preview shows human-readable orders.
  - **Apply imported** works (smart/non-smart).
  - **Save imported template** persists it into the template library.

---

# Round 17 Notes

- **UI (Portable Order Template JSON v2):** order template clipboard exchange is now truly **portable across saves**.
  - Added `ui/order_template_portable.*` which can **export templates with name-based references** (system/body/colony/jump point/ship/anomaly/wreck) and **import them by resolving those references back to IDs** in the current save.
  - Import is **fog-of-war safe** when enabled:
    - systems must be **discovered**,
    - jump points must be **surveyed**,
    - ships must be **detected**,
    - anomalies must be **discovered**.
  - Export is also fog-of-war safe: it will only emit portable names for entities visible to the viewing faction (otherwise it falls back to legacy numeric IDs).

- **UI (Orders → Order Templates):** clipboard buttons now default to portable export (with an optional legacy-IDs button):
  - **Copy current queue JSON (portable)** + **Copy legacy IDs**
  - **Copy selected template JSON (portable)** + **Copy legacy IDs**
  - **Copy compiled JSON (portable)** + **Copy legacy IDs** (both in Preview apply and Preview imported apply)

- **UI (Template exchange):** **Paste template JSON from clipboard** now accepts both:
  - legacy v1 templates (raw IDs), and
  - portable v2 templates (name-based refs).

Verification:
- In **Ship panel → Orders → Order Templates**:
  - Click **Copy selected template JSON (portable)**, paste into a text editor, confirm it includes `*_ref` fields (e.g. `body_ref`, `colony_ref`) and `nebula4x_order_template_version: 2`.
  - Start a new save (or load a different one) where the same-named systems/bodies exist.
  - Paste the JSON into **Template exchange → Paste template JSON from clipboard**:
    - it should import successfully and show the correct human-readable preview.
  - Apply to a ship with **Smart apply** enabled: compiled preview should work.

---

# Round 16 Notes

- **UI (Fleet mission planner preview):** added a new **Fleet mission planner preview** panel to the order-template previews.
  - Location:
    - **Ship panel → Orders → Order Templates → Preview apply → Fleet mission planner preview**
    - **Ship panel → Orders → Order Templates → Preview imported apply → Fleet mission planner preview**
  - Simulates applying the same template to every ship in the **currently selected fleet**, respecting:
    - **Smart apply** (auto-insert `TravelViaJump`) and
    - **Append when applying** (plan against existing queues).
  - Summary table includes per-ship **ETA**, **end fuel**, **minimum fuel**, and an at-a-glance **status** (OK / Truncated / Infeasible / Compile failed).
  - Optional **fuel reserve warnings** (configurable reserve %) to catch ships that can complete the plan but dip dangerously low.
  - Click a ship row to open a detailed per-order **Mission planner table/export** for that ship (with its own display toggles and CSV/JSON clipboard export).
  - Includes clipboard exports for the fleet summary itself:
    - **Copy summary CSV**
    - **Copy summary JSON**

Verification:
- Select a fleet (so **Apply to selected fleet** is enabled).
- Open **Order Templates → Preview apply** and expand **Fleet mission planner preview**.
  - Verify the table lists fleet members with per-ship ETA/fuel.
  - Click a ship row: a detailed planner table should appear below.
  - Click **Copy summary CSV/JSON** and paste into a text editor to confirm the export.

---

# Round 15 Notes

- **UI (Mission planner: Jump-chain collapsing):** the mission planner table can now **collapse consecutive `TravelViaJump` orders** into a single aggregated row (a "jump chain").
  - Dramatically reduces clutter when using **Smart apply (auto-route)**, which often inserts multi-jump travel legs.
  - The aggregated row shows:
    - the **order index range** it covers,
    - total Δ time across the chain,
    - final ETA/fuel, and
    - a tooltip listing each jump leg.

- **Clipboard exports improved:** CSV/JSON exports now include a little more structure so downstream tooling can distinguish collapsed rows.
  - CSV adds: `row_kind` and `index_end`.
  - JSON adds: `collapsed_jump_chains`, `row_kind`, `index_end`, and for jump chains, an array of `legs`.

- **Wired into all planner views:**
  - **Ship panel → Orders → Mission planner table/export**: new **Collapse jump chains** toggle.
  - **Orders → Order Templates → Preview apply / Preview imported apply → Mission planner table/export**: same toggle.

Verification:
- Open **Ship panel → Orders** with a ship that has (or can get) multiple consecutive `Travel via ...` orders.
  - Expand **Mission planner table/export**.
  - Toggle **Collapse jump chains** on/off and verify row count changes while total ETA/fuel remains consistent.
  - Hover the collapsed row: tooltip should list the individual jump legs.
  - Click **Copy plan CSV/JSON** and verify the export contains the new `row_kind`/`index_end` fields.

---

# Round 14 Notes

- **UI (Mission planner table + exports):** added `ui/order_plan_ui.*` which can render a per-order mission plan table and export it.
  - Works with **fog-of-war** by reusing the order pretty-printer and hiding undiscovered system names.
  - Includes one-click clipboard exports:
    - **Copy plan CSV** (great for spreadsheets / tuning routes)
    - **Copy plan JSON** (for tooling / debugging)

- **UI (Ship panel → Orders):** added an optional **Mission planner table/export** section below the editable queue.
  - Shows a scrollable per-order table including ETA, Δ time, fuel remaining, system, and planner notes.
  - Can toggle columns and max rows.

- **UI (Ship panel → Orders → Order Templates):** expanded both template preview panes (**Preview apply** and **Preview imported apply**) with a **Mission planner table/export** subpanel.
  - Lets you validate the *resulting* queue (including smart-compiled jump legs) with full step-by-step ETA/fuel detail.

Verification:
- Open **Ship panel → Orders** with a ship that has queued orders:
  - Expand **Mission planner table/export** and click **Copy plan CSV**. Paste into a text editor/spreadsheet: should include headers and one row per order.
  - Click **Copy plan JSON**: should include `{ ok, truncated, start_fuel_tons, end_fuel_tons, total_eta_days, steps: [...] }`.
- Open **Orders → Order Templates**:
  - Select a template, expand **Preview apply to this ship (ETA/fuel)**, then expand **Mission planner table/export**.
  - Paste a template JSON, expand **Preview imported apply (ETA/fuel)**, then expand **Mission planner table/export**.

---

# Round 13 Notes

- **Core (Smart order compiler generalized):**
  - Added `Simulation::compile_orders_smart()` which **smart-compiles any arbitrary order list** (not just saved templates) by inserting required `TravelViaJump` legs between steps.
  - Extended smart routing coverage to additional order types that require system-local validity:
    - `MineBody`, `BombardColony`, `SalvageWreck`, `SalvageWreckLoop`, `InvestigateAnomaly`, and `EscortShip`.
  - Improved `AttackShip` smart routing by preferring the order's `last_known_system_id` when present (avoids hard failure when the target ship isn't available).

- **Core (Apply arbitrary orders):**
  - Added `apply_orders_to_ship/fleet()` and `apply_orders_to_ship_smart/fleet_smart()` so UI/tools can apply **clipboard/imported orders directly** without saving a named template first.

- **UI (Orders → Order Templates):**
  - Added **Copy current queue JSON** for quick share/reuse without saving.
  - Added a **Preview apply** panel for the selected template:
    - shows the smart-compiled queue (when enabled),
    - includes a **clipboard export for the compiled result**, and
    - shows an **ETA/fuel preview** using the order planner.
  - Imported JSON templates can now be **applied directly to this ship or the selected fleet** (smart/non-smart), with the same preview + compiled export.

Verification:
- Open **Ship panel → Orders → Order Templates**.
- With a non-empty order queue, click **Copy current queue JSON** and paste into a text editor.
- Select a template and expand **Preview apply to this ship (ETA/fuel)**:
  - when **Smart apply** is enabled, it should show a compiled order count and allow **Copy compiled JSON**.
  - ETA/fuel preview should update based on append/replace.
- In **Template exchange (clipboard)**:
  - paste a JSON template and then click **Apply imported to this ship** (or **Apply imported to selected fleet**).
  - expand **Preview imported apply (ETA/fuel)** for the same compiled/plan preview.

---

# Round 12 Notes

- **Core (Order template exchange JSON):** added a small standalone serialization API for order templates in `nebula4x/core/serialization.*`.
  - Templates can now be exported/imported as a compact JSON snippet that reuses the same order object shape as save files ("type" + fields).
  - Import supports either an object with `{name, orders}` (optionally `nebula4x_order_template_version`) or a raw `[...]` order array.
- **UI (Order Templates library):** significantly upgraded the Ship panel -> Orders -> **Order Templates** tools:
  - Added a **selected-template preview** (first 64 orders, scrollable) using the new order pretty-printer (resolved names + fog-of-war safe).
  - Added **Copy selected template JSON** (clipboard export) for easy sharing and reuse.
  - Added a **Template exchange (clipboard)** section:
    - **Paste template JSON from clipboard** -> parses + previews.
    - Choose **Import name** and optional **Overwrite existing**.
    - **Save imported template** to persist it into the template library.

Verification:
- Open **Ship panel -> Orders -> Order Templates**.
- Select a template:
  - preview should list human-readable orders (no raw ids).
  - click **Copy selected template JSON**, paste into a text editor: should be a JSON object with `nebula4x_order_template_version`, `name`, and an `orders` array.
- Copy that JSON back into clipboard:
  - click **Paste template JSON from clipboard**: imported preview should populate.
  - click **Save imported template** and confirm it appears in the template dropdown and can be applied.

---

# Round 11 Notes

- **UI (Order pretty-printer):** added `ui/order_ui.*` which renders ship orders using resolved entity names (bodies/colonies/ships/jump points/anomalies/wrecks) instead of raw ids.
  - Respects **fog-of-war**: undiscovered systems and undetected ships fall back to `System #id` / `Ship #id` rather than leaking names.
- **UI (Tooltips):** hover any "Next order"/"First order" cell to see a full tooltip listing:
  - active queue,
  - repeat template (and remaining count), and
  - suspended original orders when auto-retreat is active.
- **UI (Automation Center / Auto-freight / Ship list / System map):** migrated all ship-order labels and order-path tooltips to the new pretty printer for dramatically more readable automation debugging.
  - Automation Center "First order" now shows the pretty string and uses the shared tooltip.
  - Auto-freight ships list now uses the shared tooltip and pretty strings.
  - Ship list (orders count tooltip) now shows the shared tooltip (includes repeat/suspended states).
  - System map order-path waypoint tooltip now shows the pretty string.

Verification:
- Open **Automation Center**, enable "First order" column, hover a ship's first order: tooltip should show the full queue and, if repeat is active, the repeat template.
- In **Logistics -> Auto-freight ships**, hover "Idle" or a next-order label: tooltip should appear with queue/repeat/suspended details.
- In the **Ship list** table, hover the **orders count** for a ship: tooltip should show the same queue/repeat/suspended detail.
- In **System Map**, enable order paths and hover a path waypoint: tooltip should show a human-readable order like "Move to Mars (Sol)" instead of ids.

---

# Round 10 Notes

- **UI (Auto-freight panel):** "Idle" now matches automation/planner semantics by using `ship_orders_is_idle_for_automation()` instead of `queue.empty()`.
  - Ships with **active repeat templates** are no longer shown as idle when their active queue is empty (the next template order is shown as `Repeat(N): ...` / `Repeat(∞): ...`).
  - Ships that are **suspended** (auto-retreat) are no longer shown as idle and will display `[Suspended]` in the order column.
- **UI (Contracts):** the ship picker now uses `ship_orders_is_idle_for_automation()` for the `[busy]` indicator, so repeat routes and suspended ships are treated as busy.

Verification:
- Enable **Auto-freight** on a ship, enable **Repeat**, clear the active queue: the Auto-freight list should show `Repeat(...)` instead of `Idle`.
- Trigger an auto-retreat suspension: the Auto-freight list should show `[Suspended]` and the Contracts ship picker should show the ship as `[busy]`.

---

# Round 9 Notes

- **UI (Automation Center):** "Idle" now matches automation/planner semantics by using `ship_orders_is_idle_for_automation()` instead of a simple `queue.empty()` check.
  - Ships with **active repeat templates** are no longer shown as idle.
  - **Suspended** ships (auto-retreat) are no longer shown as idle.
  - Optional **First order** column now displays:
    - `[Suspended] ...` when a ship is currently running an emergency plan, and
    - `Repeat(N): ...` / `Repeat(∞): ...` when the active queue is empty but the repeat template will refill.
- **Core:** removed an unused helper in `fleet_battle_forecast.cpp` to eliminate a `-Wunused-function` warning (important for warnings-as-errors builds).

Verification:
- In-game: open **Automation Center**, enable ship order **Repeat**, clear the active queue, and confirm **Idle = No** and **First order** shows `Repeat(...)`.
- Trigger an auto-retreat **suspension** and confirm the ship is **not** considered idle and shows `[Suspended]` in **First order**.
