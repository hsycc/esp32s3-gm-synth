#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

if [[ $# -lt 1 ]]; then
  echo "usage: $0 path/to/GM.sf2 [sf2-analyzer options...]" >&2
  exit 2
fi

python3 -m tools.sf2_analyzer "$@"
