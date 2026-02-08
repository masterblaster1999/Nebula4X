#!/usr/bin/env python3
"""Nebula4X content validator (Python).

This tool is intentionally dependency-free (stdlib only) so it can run anywhere.
It validates the on-disk JSON content files for basic schema correctness and
cross-reference integrity.

Usage:
  python tools/validate_content.py
  python tools/validate_content.py --root /path/to/repo

Exit code:
  0 - success
  1 - validation errors
  2 - unexpected failure (I/O, JSON parse, etc.)
"""

from __future__ import annotations

import argparse
import dataclasses
import datetime as _dt
import json
import sys
from pathlib import Path
from typing import Any, Iterable


@dataclasses.dataclass(frozen=True)
class ValidationIssue:
    file: Path
    pointer: str
    message: str

    def format(self, *, root: Path) -> str:
        rel = self.file
        try:
            rel = self.file.relative_to(root)
        except Exception:
            pass
        loc = f"{rel}"
        if self.pointer:
            loc += f":{self.pointer}"
        return f"{loc}: {self.message}"


def _json_pointer(parts: Iterable[str | int]) -> str:
    # RFC6901-ish pointer (good enough for humans).
    def esc(p: str) -> str:
        return p.replace("~", "~0").replace("/", "~1")

    out: list[str] = []
    for p in parts:
        if isinstance(p, int):
            out.append(str(p))
        else:
            out.append(esc(p))
    return "/" + "/".join(out) if out else ""


def _is_number(x: Any) -> bool:
    return isinstance(x, (int, float)) and not isinstance(x, bool)


def _load_json(path: Path) -> Any:
    # utf-8-sig tolerates accidental BOMs in content files.
    with path.open("r", encoding="utf-8-sig") as f:
        return json.load(f)


def _require_dict(issues: list[ValidationIssue], file: Path, parts: list[Any], v: Any, what: str) -> dict[str, Any] | None:
    if not isinstance(v, dict):
        issues.append(ValidationIssue(file, _json_pointer(parts), f"{what} must be an object"))
        return None
    # Ensure keys are strings to avoid weird JSON.
    for k in v.keys():
        if not isinstance(k, str):
            issues.append(ValidationIssue(file, _json_pointer(parts), f"{what} keys must be strings"))
            return None
    return v  # type: ignore[return-value]


def _require_list(issues: list[ValidationIssue], file: Path, parts: list[Any], v: Any, what: str) -> list[Any] | None:
    if not isinstance(v, list):
        issues.append(ValidationIssue(file, _json_pointer(parts), f"{what} must be an array"))
        return None
    return v


def _require_str(issues: list[ValidationIssue], file: Path, parts: list[Any], v: Any, what: str) -> str | None:
    if not isinstance(v, str) or not v:
        issues.append(ValidationIssue(file, _json_pointer(parts), f"{what} must be a non-empty string"))
        return None
    return v


def _require_int(issues: list[ValidationIssue], file: Path, parts: list[Any], v: Any, what: str, *, min_value: int | None = None) -> int | None:
    if not isinstance(v, int) or isinstance(v, bool):
        issues.append(ValidationIssue(file, _json_pointer(parts), f"{what} must be an integer"))
        return None
    if min_value is not None and v < min_value:
        issues.append(ValidationIssue(file, _json_pointer(parts), f"{what} must be >= {min_value}"))
        return None
    return v


def _require_number(issues: list[ValidationIssue], file: Path, parts: list[Any], v: Any, what: str, *, min_value: float | None = None) -> float | None:
    if not _is_number(v):
        issues.append(ValidationIssue(file, _json_pointer(parts), f"{what} must be a number"))
        return None
    vv = float(v)
    if min_value is not None and vv < min_value:
        issues.append(ValidationIssue(file, _json_pointer(parts), f"{what} must be >= {min_value}"))
        return None
    return vv


def _require_bool(issues: list[ValidationIssue], file: Path, parts: list[Any], v: Any, what: str) -> bool | None:
    if not isinstance(v, bool):
        issues.append(ValidationIssue(file, _json_pointer(parts), f"{what} must be true/false"))
        return None
    return v


def validate_resources(root: Path, *, issues: list[ValidationIssue]) -> dict[str, dict[str, Any]] | None:
    path = root / "data" / "blueprints" / "resources.json"
    if not path.exists():
        issues.append(ValidationIssue(path, "", "file not found"))
        return None

    try:
        data = _load_json(path)
    except Exception as e:
        issues.append(ValidationIssue(path, "", f"failed to parse JSON: {e}"))
        return None

    top = _require_dict(issues, path, [], data, "root")
    if top is None:
        return None

    _require_int(issues, path, ["version"], top.get("version"), "version", min_value=1)

    resources = _require_dict(issues, path, ["resources"], top.get("resources"), "resources")
    if resources is None:
        return None

    for rid, robj in resources.items():
        rparts = ["resources", rid]
        if not isinstance(rid, str) or not rid:
            issues.append(ValidationIssue(path, _json_pointer(rparts), "resource id must be a non-empty string"))
            continue
        robj_d = _require_dict(issues, path, rparts, robj, f"resource '{rid}'")
        if robj_d is None:
            continue
        _require_str(issues, path, rparts + ["name"], robj_d.get("name"), "name")
        _require_str(issues, path, rparts + ["category"], robj_d.get("category"), "category")
        _require_bool(issues, path, rparts + ["mineable"], robj_d.get("mineable"), "mineable")
        _require_number(
            issues,
            path,
            rparts + ["salvage_research_rp_per_ton"],
            robj_d.get("salvage_research_rp_per_ton"),
            "salvage_research_rp_per_ton",
            min_value=0.0,
        )

    return resources  # type: ignore[return-value]


_ALLOWED_COMPONENT_TYPES: set[str] = {
    "armor",
    "cargo",
    "colony_module",
    "engine",
    "fuel_tank",
    "mining",
    "reactor",
    "sensor",
    "shield",
    "troop_bay",
    "weapon",
}


_ALLOWED_SHIP_ROLES: set[str] = {
    "freighter",
    "surveyor",
    "combatant",
    "unknown",
}


def _validate_resource_map(
    issues: list[ValidationIssue],
    file: Path,
    parts: list[Any],
    v: Any,
    what: str,
    resource_ids: set[str],
) -> None:
    m = _require_dict(issues, file, parts, v, what)
    if m is None:
        return
    for k, amt in m.items():
        if not isinstance(k, str) or not k:
            issues.append(ValidationIssue(file, _json_pointer(parts), f"{what} resource keys must be non-empty strings"))
            continue
        if k not in resource_ids:
            issues.append(ValidationIssue(file, _json_pointer(parts + [k]), f"unknown resource '{k}'"))
        _require_number(issues, file, parts + [k], amt, f"{what}['{k}']", min_value=0.0)


def validate_blueprints(
    root: Path,
    *,
    issues: list[ValidationIssue],
    resource_ids: set[str],
) -> tuple[dict[str, dict[str, Any]] | None, dict[str, dict[str, Any]] | None]:
    path = root / "data" / "blueprints" / "starting_blueprints.json"
    if not path.exists():
        issues.append(ValidationIssue(path, "", "file not found"))
        return None, None

    try:
        data = _load_json(path)
    except Exception as e:
        issues.append(ValidationIssue(path, "", f"failed to parse JSON: {e}"))
        return None, None

    top = _require_dict(issues, path, [], data, "root")
    if top is None:
        return None, None

    _require_int(issues, path, ["version"], top.get("version"), "version", min_value=1)

    # Optional includes.
    inc = top.get("include")
    if inc is not None:
        inc_list = _require_list(issues, path, ["include"], inc, "include")
        if inc_list is not None:
            for i, entry in enumerate(inc_list):
                s = _require_str(issues, path, ["include", i], entry, "include entry")
                if s is None:
                    continue
                inc_path = (path.parent / s).resolve()
                if not inc_path.exists():
                    issues.append(ValidationIssue(path, _json_pointer(["include", i]), f"included file not found: {s}"))

    comps = _require_dict(issues, path, ["components"], top.get("components"), "components")
    if comps is None:
        return None, None

    components: dict[str, dict[str, Any]] = {}

    for cid, cobj in comps.items():
        cparts = ["components", cid]
        if not isinstance(cid, str) or not cid:
            issues.append(ValidationIssue(path, _json_pointer(cparts), "component id must be a non-empty string"))
            continue
        cdict = _require_dict(issues, path, cparts, cobj, f"component '{cid}'")
        if cdict is None:
            continue
        name = _require_str(issues, path, cparts + ["name"], cdict.get("name"), "name")
        ctype = _require_str(issues, path, cparts + ["type"], cdict.get("type"), "type")
        if ctype is not None and ctype not in _ALLOWED_COMPONENT_TYPES:
            issues.append(ValidationIssue(path, _json_pointer(cparts + ["type"]), f"unknown component type '{ctype}'"))

        mass = cdict.get("mass_tons")
        if mass is not None:
            _require_number(issues, path, cparts + ["mass_tons"], mass, "mass_tons", min_value=0.0)

        # Minimal per-type required fields.
        if ctype == "engine":
            _require_number(issues, path, cparts + ["speed_km_s"], cdict.get("speed_km_s"), "speed_km_s", min_value=0.001)
            _require_number(
                issues,
                path,
                cparts + ["fuel_use_per_mkm"],
                cdict.get("fuel_use_per_mkm"),
                "fuel_use_per_mkm",
                min_value=0.0,
            )
        elif ctype == "cargo":
            _require_number(issues, path, cparts + ["cargo_tons"], cdict.get("cargo_tons"), "cargo_tons", min_value=0.001)
        elif ctype == "mining":
            _require_number(
                issues,
                path,
                cparts + ["mining_tons_per_day"],
                cdict.get("mining_tons_per_day"),
                "mining_tons_per_day",
                min_value=0.001,
            )
        elif ctype == "fuel_tank":
            _require_number(
                issues,
                path,
                cparts + ["fuel_capacity_tons"],
                cdict.get("fuel_capacity_tons"),
                "fuel_capacity_tons",
                min_value=0.001,
            )
        elif ctype == "colony_module":
            _require_number(
                issues,
                path,
                cparts + ["colony_capacity_millions"],
                cdict.get("colony_capacity_millions"),
                "colony_capacity_millions",
                min_value=0.001,
            )
        elif ctype == "troop_bay":
            _require_int(issues, path, cparts + ["troop_capacity"], cdict.get("troop_capacity"), "troop_capacity", min_value=1)
        elif ctype == "sensor":
            # Sensors can be range, ecm, eccm, etc.
            if "range_mkm" in cdict:
                _require_number(issues, path, cparts + ["range_mkm"], cdict.get("range_mkm"), "range_mkm", min_value=0.001)
            if "ecm_strength" in cdict:
                _require_number(
                    issues,
                    path,
                    cparts + ["ecm_strength"],
                    cdict.get("ecm_strength"),
                    "ecm_strength",
                    min_value=0.0,
                )
            if "eccm_strength" in cdict:
                _require_number(
                    issues,
                    path,
                    cparts + ["eccm_strength"],
                    cdict.get("eccm_strength"),
                    "eccm_strength",
                    min_value=0.0,
                )
        elif ctype == "reactor":
            _require_number(issues, path, cparts + ["power"], cdict.get("power"), "power", min_value=0.001)
        elif ctype == "weapon":
            # Accept at least one supported weapon field group.
            has_beam = "damage" in cdict or "weapon_range_mkm" in cdict
            has_missile = "missile_damage" in cdict or "missile_range_mkm" in cdict
            has_pd = "point_defense_damage" in cdict or "point_defense_range_mkm" in cdict
            if not (has_beam or has_missile or has_pd):
                issues.append(ValidationIssue(path, _json_pointer(cparts), "weapon must define beam/missile/point-defense fields"))
            if has_beam:
                _require_number(issues, path, cparts + ["damage"], cdict.get("damage"), "damage", min_value=0.0)
                _require_number(
                    issues,
                    path,
                    cparts + ["weapon_range_mkm"],
                    cdict.get("weapon_range_mkm"),
                    "weapon_range_mkm",
                    min_value=0.0,
                )
            if has_missile:
                _require_number(
                    issues,
                    path,
                    cparts + ["missile_damage"],
                    cdict.get("missile_damage"),
                    "missile_damage",
                    min_value=0.0,
                )
                _require_number(
                    issues,
                    path,
                    cparts + ["missile_range_mkm"],
                    cdict.get("missile_range_mkm"),
                    "missile_range_mkm",
                    min_value=0.0,
                )
                _require_number(
                    issues,
                    path,
                    cparts + ["missile_speed_mkm_per_day"],
                    cdict.get("missile_speed_mkm_per_day"),
                    "missile_speed_mkm_per_day",
                    min_value=0.0,
                )
                _require_int(
                    issues,
                    path,
                    cparts + ["missile_reload_days"],
                    cdict.get("missile_reload_days"),
                    "missile_reload_days",
                    min_value=0,
                )
                _require_int(
                    issues,
                    path,
                    cparts + ["missile_ammo"],
                    cdict.get("missile_ammo"),
                    "missile_ammo",
                    min_value=0,
                )
            if has_pd:
                _require_number(
                    issues,
                    path,
                    cparts + ["point_defense_damage"],
                    cdict.get("point_defense_damage"),
                    "point_defense_damage",
                    min_value=0.0,
                )
                _require_number(
                    issues,
                    path,
                    cparts + ["point_defense_range_mkm"],
                    cdict.get("point_defense_range_mkm"),
                    "point_defense_range_mkm",
                    min_value=0.0,
                )
        elif ctype == "armor":
            if "hp_bonus" in cdict:
                _require_number(issues, path, cparts + ["hp_bonus"], cdict.get("hp_bonus"), "hp_bonus", min_value=0.0)
            if "signature_multiplier" in cdict:
                _require_number(
                    issues,
                    path,
                    cparts + ["signature_multiplier"],
                    cdict.get("signature_multiplier"),
                    "signature_multiplier",
                    min_value=0.0,
                )
        elif ctype == "shield":
            _require_number(issues, path, cparts + ["shield_hp"], cdict.get("shield_hp"), "shield_hp", min_value=0.001)
            _require_number(
                issues,
                path,
                cparts + ["shield_regen_per_day"],
                cdict.get("shield_regen_per_day"),
                "shield_regen_per_day",
                min_value=0.0,
            )

        if name is not None:
            components[cid] = cdict

    # Designs.
    designs = _require_list(issues, path, ["designs"], top.get("designs"), "designs")
    design_ids: set[str] = set()
    if designs is not None:
        for i, dobj in enumerate(designs):
            dparts = ["designs", i]
            d = _require_dict(issues, path, dparts, dobj, "design")
            if d is None:
                continue
            did = _require_str(issues, path, dparts + ["id"], d.get("id"), "id")
            if did is not None:
                if did in design_ids:
                    issues.append(ValidationIssue(path, _json_pointer(dparts + ["id"]), f"duplicate design id '{did}'"))
                design_ids.add(did)
            _require_str(issues, path, dparts + ["name"], d.get("name"), "name")
            role = _require_str(issues, path, dparts + ["role"], d.get("role"), "role")
            if role is not None and role not in _ALLOWED_SHIP_ROLES:
                issues.append(ValidationIssue(path, _json_pointer(dparts + ["role"]), f"unknown ship role '{role}'"))
            comp_list = _require_list(issues, path, dparts + ["components"], d.get("components"), "components")
            if comp_list is not None:
                for j, comp in enumerate(comp_list):
                    cid2 = _require_str(issues, path, dparts + ["components", j], comp, "component id")
                    if cid2 is None:
                        continue
                    if cid2 not in components:
                        issues.append(ValidationIssue(path, _json_pointer(dparts + ["components", j]), f"unknown component '{cid2}'"))

    # Installations.
    insts = _require_dict(issues, path, ["installations"], top.get("installations"), "installations")
    if insts is None:
        return components, None

    installations: dict[str, dict[str, Any]] = {}

    for iid, iobj in insts.items():
        iparts = ["installations", iid]
        if not isinstance(iid, str) or not iid:
            issues.append(ValidationIssue(path, _json_pointer(iparts), "installation id must be a non-empty string"))
            continue
        inst = _require_dict(issues, path, iparts, iobj, f"installation '{iid}'")
        if inst is None:
            continue
        _require_str(issues, path, iparts + ["name"], inst.get("name"), "name")

        if "construction_cost" in inst:
            _require_number(issues, path, iparts + ["construction_cost"], inst.get("construction_cost"), "construction_cost", min_value=0.0)

        # Resource maps.
        if "build_costs" in inst:
            _validate_resource_map(issues, path, iparts + ["build_costs"], inst.get("build_costs"), "build_costs", resource_ids)
        if "build_costs_per_ton" in inst:
            _validate_resource_map(
                issues, path, iparts + ["build_costs_per_ton"], inst.get("build_costs_per_ton"), "build_costs_per_ton", resource_ids
            )
        if "produces" in inst:
            _validate_resource_map(issues, path, iparts + ["produces"], inst.get("produces"), "produces", resource_ids)
        if "consumes" in inst:
            _validate_resource_map(issues, path, iparts + ["consumes"], inst.get("consumes"), "consumes", resource_ids)

        installations[iid] = inst

    return components, installations


_ALLOWED_FACTION_OUTPUT_BONUS_KEYS: set[str] = {
    "all",
    "construction",
    "industry",
    "mining",
    "research",
    "shipyard",
    "terraforming",
    "troop_training",
}


def validate_tech_tree(
    root: Path,
    *,
    issues: list[ValidationIssue],
    component_ids: set[str],
    installation_ids: set[str],
) -> None:
    path = root / "data" / "tech" / "tech_tree.json"
    if not path.exists():
        issues.append(ValidationIssue(path, "", "file not found"))
        return

    try:
        data = _load_json(path)
    except Exception as e:
        issues.append(ValidationIssue(path, "", f"failed to parse JSON: {e}"))
        return

    top = _require_dict(issues, path, [], data, "root")
    if top is None:
        return

    _require_int(issues, path, ["version"], top.get("version"), "version", min_value=1)

    techs = _require_list(issues, path, ["techs"], top.get("techs"), "techs")
    if techs is None:
        return

    tech_by_id: dict[str, dict[str, Any]] = {}
    tech_index_by_id: dict[str, int] = {}

    for i, tobj in enumerate(techs):
        tparts = ["techs", i]
        t = _require_dict(issues, path, tparts, tobj, "tech")
        if t is None:
            continue
        tid = _require_str(issues, path, tparts + ["id"], t.get("id"), "id")
        if tid is None:
            continue
        if tid in tech_by_id:
            issues.append(ValidationIssue(path, _json_pointer(tparts + ["id"]), f"duplicate tech id '{tid}'"))
            continue
        tech_by_id[tid] = t
        tech_index_by_id[tid] = i

        _require_str(issues, path, tparts + ["name"], t.get("name"), "name")
        _require_int(issues, path, tparts + ["cost"], t.get("cost"), "cost", min_value=0)

        prereqs = _require_list(issues, path, tparts + ["prereqs"], t.get("prereqs"), "prereqs")
        if prereqs is not None:
            for j, pre in enumerate(prereqs):
                _require_str(issues, path, tparts + ["prereqs", j], pre, "prereq")

        effects = _require_list(issues, path, tparts + ["effects"], t.get("effects"), "effects")
        if effects is not None:
            for j, eobj in enumerate(effects):
                eparts = tparts + ["effects", j]
                e = _require_dict(issues, path, eparts, eobj, "effect")
                if e is None:
                    continue
                etype = _require_str(issues, path, eparts + ["type"], e.get("type"), "type")
                if etype is None:
                    continue
                if etype not in {"unlock_component", "unlock_installation", "faction_output_bonus"}:
                    issues.append(ValidationIssue(path, _json_pointer(eparts + ["type"]), f"unknown effect type '{etype}'"))
                    continue

                if etype in {"unlock_component", "unlock_installation"}:
                    val = _require_str(issues, path, eparts + ["value"], e.get("value"), "value")
                    if val is None:
                        continue
                    if etype == "unlock_component" and val not in component_ids:
                        issues.append(ValidationIssue(path, _json_pointer(eparts + ["value"]), f"unknown component '{val}'"))
                    if etype == "unlock_installation" and val not in installation_ids:
                        issues.append(ValidationIssue(path, _json_pointer(eparts + ["value"]), f"unknown installation '{val}'"))
                elif etype == "faction_output_bonus":
                    key = _require_str(issues, path, eparts + ["value"], e.get("value"), "value")
                    if key is not None and key not in _ALLOWED_FACTION_OUTPUT_BONUS_KEYS:
                        issues.append(ValidationIssue(path, _json_pointer(eparts + ["value"]), f"unknown output bonus key '{key}'"))
                    _require_number(issues, path, eparts + ["amount"], e.get("amount"), "amount", min_value=0.0)

    # Validate prereqs exist and detect cycles.
    for tid, t in tech_by_id.items():
        prereqs = t.get("prereqs")
        if not isinstance(prereqs, list):
            continue
        for j, pre in enumerate(prereqs):
            if not isinstance(pre, str):
                continue
            if pre not in tech_by_id:
                idx = tech_index_by_id.get(tid)
                ptr = _json_pointer(["techs", idx if idx is not None else tid, "prereqs", j])
                issues.append(ValidationIssue(path, ptr, f"unknown prereq tech '{pre}'"))

    # Cycle detection via DFS.
    visiting: set[str] = set()
    visited: set[str] = set()

    def dfs(tid2: str, stack: list[str]) -> None:
        if tid2 in visited:
            return
        if tid2 in visiting:
            cycle = stack[stack.index(tid2) :] + [tid2]
            issues.append(ValidationIssue(path, "", f"tech prereq cycle detected: {' -> '.join(cycle)}"))
            return
        visiting.add(tid2)
        stack.append(tid2)
        prereqs2 = tech_by_id.get(tid2, {}).get("prereqs", [])
        if isinstance(prereqs2, list):
            for pre in prereqs2:
                if isinstance(pre, str) and pre in tech_by_id:
                    dfs(pre, stack)
        stack.pop()
        visiting.remove(tid2)
        visited.add(tid2)

    for tid2 in tech_by_id.keys():
        if tid2 not in visited:
            dfs(tid2, [])


def validate_settings(root: Path, *, issues: list[ValidationIssue]) -> None:
    path = root / "data" / "settings.json"
    if not path.exists():
        issues.append(ValidationIssue(path, "", "file not found"))
        return

    try:
        data = _load_json(path)
    except Exception as e:
        issues.append(ValidationIssue(path, "", f"failed to parse JSON: {e}"))
        return

    top = _require_dict(issues, path, [], data, "root")
    if top is None:
        return

    # Very lightweight sanity checks.
    if "startingScenario" in top:
        _require_str(issues, path, ["startingScenario"], top.get("startingScenario"), "startingScenario")

    sim = top.get("sim")
    if sim is not None:
        sim_d = _require_dict(issues, path, ["sim"], sim, "sim")
        if sim_d is not None:
            if "startDate" in sim_d:
                s = _require_str(issues, path, ["sim", "startDate"], sim_d.get("startDate"), "startDate")
                if s is not None:
                    try:
                        _dt.date.fromisoformat(s)
                    except Exception:
                        issues.append(ValidationIssue(path, _json_pointer(["sim", "startDate"]), "startDate must be YYYY-MM-DD"))
            if "secondsPerDay" in sim_d:
                _require_int(issues, path, ["sim", "secondsPerDay"], sim_d.get("secondsPerDay"), "secondsPerDay", min_value=1)


def validate_all(root: Path) -> list[ValidationIssue]:
    issues: list[ValidationIssue] = []

    resources = validate_resources(root, issues=issues)
    resource_ids = set(resources.keys()) if resources else set()

    components, installations = validate_blueprints(root, issues=issues, resource_ids=resource_ids)
    component_ids = set(components.keys()) if components else set()
    installation_ids = set(installations.keys()) if installations else set()

    validate_tech_tree(root, issues=issues, component_ids=component_ids, installation_ids=installation_ids)
    validate_settings(root, issues=issues)

    return issues


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description="Validate Nebula4X JSON content files (stdlib-only).")
    ap.add_argument(
        "--root",
        type=Path,
        default=Path(__file__).resolve().parents[1],
        help="Repo root (defaults to parent of tools/)",
    )
    args = ap.parse_args(argv)

    root = args.root.resolve()
    issues = validate_all(root)

    if issues:
        for issue in issues:
            print(issue.format(root=root), file=sys.stderr)
        print(f"\n{len(issues)} issue(s) found.", file=sys.stderr)
        return 1

    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main(sys.argv[1:]))
    except SystemExit:
        raise
    except Exception as e:
        print(f"Unexpected failure: {e}", file=sys.stderr)
        raise SystemExit(2)
