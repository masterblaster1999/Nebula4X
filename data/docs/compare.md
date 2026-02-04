# Compare / Diff

The **Compare / Diff** window helps you answer questions like:

- *“What changed on this ship after repairs?”*
- *“How is Colony A different from Colony B?”*
- *“What exactly is different between these two JSON entities?”*

It compares two entities from the **live game JSON** (or a snapshot) and presents:

- A readable, flattened diff of scalar fields (`number`, `string`, `bool`, `null`)
- Optional container summaries (object/array sizes)
- A **JSON Merge Patch (RFC 7396)** export (A → B) for debugging/save-edit workflows

---

## Open it

- **Tools → Compare / Diff (Entities)**
- **Hotkey:** `Ctrl+Shift+X` (rebindable)
- **OmniSearch:** type `compare` and run **Compare / Diff**

---

## Pick entities A and B

Each slot supports:

- **Use Selected**: uses the current selection (ship / colony / body / system)
- **Pick…**: browse/search the indexed entities in the current game state
- **ID field**: paste/enter an entity id directly

If the entity kind supports it (`ships`, `colonies`, `bodies`, `systems`), the **Jump** button will focus the game UI on that entity.

---

## Snapshots

Snapshots “freeze” one side of the comparison:

1. Set slot A (or B) to an entity
2. Click **Snapshot**
3. Make changes in-game
4. Compare the snapshot vs the live state

You can **Copy Snapshot JSON** to the clipboard for external inspection.

---

## Diff options

- **Show unchanged**: include fields that are equal on both sides
- **Include containers**: show `{N}` for objects and `[N]` for arrays to quickly detect structural changes
- **Case sensitive filter**: search paths/values with exact casing
- **Max depth / node budget**: limits flattening of deep/large structures to keep the UI responsive
- **Max value chars**: truncates long strings

---

## Export: JSON Merge Patch (A → B)

Under **Export**, you can generate a JSON Merge Patch describing how to transform A into B.

This is useful for debugging and save-edit tooling because it is:

- Small (only changed keys)
- Human-readable
- Easy to apply with standard merge-patch utilities

Click **Copy Merge Patch** to put it on the clipboard.
