#!/usr/bin/env python3
"""Create a clean Nebula4X source zip (stdlib-only).

This script exists because the `zip` CLI is not always available (notably on
Windows runners). It intentionally uses only the Python standard library.

Default exclusions target build artifacts and common editor/OS junk.

Usage:
  python tools/make_release_zip.py
  python tools/make_release_zip.py --output nebula4x_source.zip
  python tools/make_release_zip.py --root /path/to/repo --output out.zip

Exit code:
  0 - success
  1 - invalid arguments
  2 - unexpected failure
"""

from __future__ import annotations

import argparse
import os
import stat
import sys
import zipfile
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


@dataclass(frozen=True)
class ExcludeSpec:
    """Configurable exclude rules.

    The defaults here match .gitignore expectations and the existing
    tools/make_release_zip.sh exclude list.
    """

    exclude_top_dirs: frozenset[str] = frozenset({"out", "build", ".git"})
    exclude_any_dirs: frozenset[str] = frozenset({".idea", ".vscode", "__pycache__"})
    exclude_filenames: frozenset[str] = frozenset({".DS_Store", "Thumbs.db"})
    exclude_suffixes: frozenset[str] = frozenset({".zip", ".log", ".pyc"})

    def should_exclude(self, rel: Path) -> bool:
        parts = rel.parts
        if not parts:
            return False
        if parts[0] in self.exclude_top_dirs:
            return True
        for p in parts:
            if p in self.exclude_any_dirs:
                return True
        name = rel.name
        if name in self.exclude_filenames:
            return True
        for suf in self.exclude_suffixes:
            if name.endswith(suf):
                return True
        return False


def iter_included_files(root: Path, *, output: Path, excludes: ExcludeSpec) -> list[Path]:
    """Return a sorted list of included files relative to `root`."""

    root = root.resolve()
    output_resolved = output.resolve()

    files: list[Path] = []
    for p in root.rglob("*"):
        # Follow existing behavior: include only regular files.
        if not p.is_file():
            continue
        try:
            rel = p.relative_to(root)
        except Exception:
            # Should be impossible, but be safe.
            continue

        # Never include the output zip itself (even if it already exists).
        try:
            if p.resolve() == output_resolved:
                continue
        except Exception:
            # If the file disappears mid-walk, skip.
            continue

        if excludes.should_exclude(rel):
            continue

        files.append(rel)

    files.sort(key=lambda x: x.as_posix())
    return files


def _zipinfo_for_file(root: Path, rel: Path) -> zipfile.ZipInfo:
    p = root / rel
    st = p.stat()

    # Use filesystem timestamps (zipfile requires >= 1980-01-01).
    mtime = int(st.st_mtime)
    dt = zipfile.ZipInfo.from_file(p, arcname=rel.as_posix()).date_time
    zi = zipfile.ZipInfo(filename=rel.as_posix(), date_time=dt)

    # Preserve permission bits when possible.
    mode = stat.S_IMODE(st.st_mode)
    zi.external_attr = (mode & 0xFFFF) << 16

    # Store symlinks as their target content? For now: follow filesystem and
    # include as a file if it resolves to a file.
    # (Symlinks in repo zips are uncommon and may be platform-dependent.)

    # Compression type gets applied by ZipFile.writestr/write.
    _ = mtime  # keep local var referenced for clarity
    return zi


def create_source_zip(
    root: Path,
    output: Path,
    *,
    excludes: ExcludeSpec | None = None,
) -> int:
    """Create a source zip and return number of files written."""

    root = root.resolve()
    output = output.resolve()
    excludes = excludes or ExcludeSpec()

    if not root.exists() or not root.is_dir():
        raise FileNotFoundError(f"root directory not found: {root}")

    # Build file list before creating the output file to avoid races where the
    # newly created output is discovered during the walk.
    files = iter_included_files(root, output=output, excludes=excludes)

    output.parent.mkdir(parents=True, exist_ok=True)

    # Ensure deterministic ordering of entries.
    with zipfile.ZipFile(output, mode="w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
        for rel in files:
            src = root / rel
            zi = _zipinfo_for_file(root, rel)
            with src.open("rb") as f:
                zf.writestr(zi, f.read())

    return len(files)


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description="Create a clean Nebula4X source zip (stdlib-only).")
    ap.add_argument(
        "--root",
        type=Path,
        default=Path(__file__).resolve().parents[1],
        help="Repo root (defaults to parent of tools/)",
    )
    ap.add_argument(
        "--output",
        type=Path,
        default=Path("nebula4x_source.zip"),
        help="Output zip path (default: nebula4x_source.zip)",
    )
    args = ap.parse_args(argv)

    root = args.root.resolve()

    # Match historical behavior: treat relative output as relative to root.
    output = args.output
    if not output.is_absolute():
        output = (root / output).resolve()

    try:
        n = create_source_zip(root, output)
    except FileNotFoundError as e:
        print(str(e), file=sys.stderr)
        return 1

    rel_out = output
    try:
        rel_out = output.relative_to(root)
    except Exception:
        pass

    print(f"Wrote {rel_out} ({n} file(s))")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main(sys.argv[1:]))
    except SystemExit:
        raise
    except Exception as e:
        print(f"Unexpected failure: {e}", file=sys.stderr)
        raise SystemExit(2)
