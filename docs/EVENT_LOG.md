# Event log exports

Nebula4X keeps a **persistent simulation event log** (build/research/jump/combat/etc.).
The log is saved with the game and can be inspected in both the UI (**Log** tab) and the CLI.

This doc focuses on **exporting** and **scripting** around the event log.

## Formats

### CSV

- Designed for spreadsheets (Excel / Google Sheets) and quick plotting.
- Header row is included.
- Strings are CSV-escaped (quoted when needed; internal quotes doubled).

CLI:

```bash
./build/nebula4x_cli --export-events-csv events.csv --events-level all
```

UI:

- Log tab → **Export CSV**

### JSON

- A single JSON document containing an **array** of event objects.
- Pretty-printed with indentation.

CLI:

```bash
./build/nebula4x_cli --export-events-json events.json --events-level all
```

UI:

- Log tab → **Export JSON**

### JSONL / NDJSON

- **One JSON object per line** (newline-delimited JSON).
- Great for streaming, `grep`, and `jq`.

CLI:

```bash
./build/nebula4x_cli --export-events-jsonl events.jsonl --events-level all

# stream to stdout (script-friendly)
./build/nebula4x_cli --load save.json --quiet --export-events-jsonl - --events-level warn,error | jq -c '.'
```

UI:

- Log tab → **Export JSONL**


### Summary JSON

- A single JSON document containing **counts** for the filtered event set.
- Includes day+date range, counts by level, and counts by category.
- Useful for scripts and CI sanity checks.

CLI:

```bash
./build/nebula4x_cli --events-summary-json events_summary.json --events-level all

# stream to stdout (script-friendly)
./build/nebula4x_cli --load save.json --quiet --events-level warn,error --events-summary-json - | jq .
```


## Event object schema

The JSON and JSONL exports use the same fields.

| Field | Type | Description |
|---|---:|---|
| `day` | int | Simulation day (days since epoch). |
| `date` | string | ISO date `YYYY-MM-DD` for convenience. |
| `seq` | int | Monotonic event sequence number. |
| `level` | string | `info` / `warn` / `error`. |
| `category` | string | `general` / `research` / `shipyard` / `construction` / `movement` / `combat` / `intel` / `exploration`. |
| `faction_id` | int | Primary faction id (or `0` if not applicable). |
| `faction` | string | Resolved faction name (best effort; empty if unknown). |
| `faction_id2` | int | Secondary faction id (optional context; `0` if not applicable). |
| `faction2` | string | Resolved secondary faction name. |
| `system_id` | int | Related system id (`0` if not applicable). |
| `system` | string | Resolved system name. |
| `ship_id` | int | Related ship id (`0` if not applicable). |
| `ship` | string | Resolved ship name. |
| `colony_id` | int | Related colony id (`0` if not applicable). |
| `colony` | string | Resolved colony name. |
| `message` | string | Human-readable event message. |

Example object:

```json
{
  "day": 10,
  "date": "2200-01-11",
  "seq": 5,
  "level": "warn",
  "category": "movement",
  "faction_id": 1,
  "faction": "Terrans",
  "faction_id2": 0,
  "faction2": "",
  "system_id": 10,
  "system": "Sol",
  "ship_id": 42,
  "ship": "SC-1",
  "colony_id": 7,
  "colony": "Earth",
  "message": "Jump transit complete"
}
```

## CLI filters

All exports support the same filter flags:

- `--events-last N` — keep only the last N matching events
- `--events-category NAME`
- `--events-level LEVELS` — `all` or comma-separated list (e.g. `warn,error`)
- `--events-faction X` — id or exact name (case-insensitive)
- `--events-system X` — id or exact name (case-insensitive)
- `--events-ship X` — id or exact name (case-insensitive)
- `--events-colony X` — id or exact name (case-insensitive)
- `--events-contains TEXT` — case-insensitive substring
- `--events-since X` / `--events-until X` — day number or ISO date `YYYY-MM-DD`

## Scripting notes

- For machine-readable exports to stdout (`PATH = '-'`), the CLI routes non-essential status output to **stderr** automatically.
- `--quiet` is still recommended to suppress most status output entirely.
- When `--until-event` is used with either `--quiet` **or** a stdout export, the **hit/no-hit** status line is printed to **stderr** (not stdout).
