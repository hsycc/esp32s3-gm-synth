#!/usr/bin/env bash
set -euo pipefail

if ! command -v idf.py >/dev/null 2>&1; then
  echo "idf.py not found. Activate ESP-IDF v6.1 first (EIM/export script)." >&2
  exit 2
fi

if [[ ! -f sdkconfig ]]; then
  idf.py set-target esp32s3
fi

idf.py build
