#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${1:-nebula4x_source.zip}"

cd "$ROOT_DIR"

# Exclude build artifacts and git metadata.
zip -r "$OUT" . -x "build/*" -x ".git/*" -x "*.zip" 

echo "Wrote $OUT"
