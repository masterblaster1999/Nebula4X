#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${1:-nebula4x_source.zip}"

cd "$ROOT_DIR"

# Prefer the cross-platform Python implementation when available.
if command -v python3 >/dev/null 2>&1 && [[ -f "tools/make_release_zip.py" ]]; then
  python3 tools/make_release_zip.py --root "$ROOT_DIR" --output "$OUT"
  exit 0
fi

# Exclude build artifacts and git metadata.
zip -r "$OUT" . \
  -x "build/*" -x "build/**" \
  -x "out/*" -x "out/**" \
  -x ".git/*" -x ".git/**" \
  -x ".idea/*" -x ".idea/**" \
  -x ".vscode/*" -x ".vscode/**" \
  -x "*.zip" \
  -x "*.log" \
  -x "__pycache__/*" -x "__pycache__/**" \
  -x "*.pyc" \
  -x ".DS_Store" \
  -x "Thumbs.db"

echo "Wrote $OUT"
