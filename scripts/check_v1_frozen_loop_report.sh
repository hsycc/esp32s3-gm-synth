#!/usr/bin/env bash
# Keep binary identity separate from semantic policy identity across runner upgrades.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
REPORT="$(realpath "${1:?usage: check_v1_frozen_loop_report.sh report.json}")"
ASSET="$(realpath assets/sf2/full/GeneralUser-GS.sf2)"
BASE=6d954a7bc200cd0ec300873e7e106fff2a87a692
OUT="${V1_POLICY_CONTROL_DIR:-build/v1-loop-frozen-control}"
mkdir -p "$OUT/source";OUT="$(cd "$OUT" && pwd)"
git archive "$BASE" firmware scripts tools | tar -x -C "$OUT/source"
(
  cd "$OUT/source"
  V1_LOOP_BUILD_DIR="$OUT/exporter" bash scripts/build_v1_loop_exporter.sh
  python3 -m tools.v1_effective_loop_survey "$ASSET" \
    --exporter "$OUT/exporter/export_prepared_loops" --limit 4096 \
    --output "$OUT/reference.json"
)
# Full byte comparison: not merely matching candidate counts or stripping metadata.
cmp "$REPORT" "$OUT/reference.json"
python3 - "$REPORT" "$OUT/reference.json" <<'PY'
import hashlib,json,sys
from pathlib import Path
current,reference=map(Path,sys.argv[1:])
data=json.loads(current.read_bytes())
exporter=data.pop('exporter_sha256')
# This anchor was calculated from the verified 6d954a7b artifact. Every field,
# candidate, metric, rejection, geometry and approval flag is covered; only the
# separately checked, compiler-dependent executable hash is omitted here.
canonical=json.dumps(data,sort_keys=True,separators=(',',':'),ensure_ascii=True,allow_nan=False).encode()
semantic=hashlib.sha256(canonical).hexdigest()
expected='4121172daa431c85b63c8e68cb0752c35da0b6bbcc61271bbf319bf70025c41b'
if semantic != expected:
    raise SystemExit('historical frozen policy content changed: '+semantic)
evidence=dict(baseline_commit='6d954a7bc200cd0ec300873e7e106fff2a87a692',
              exact_same_toolchain_report_equal=current.read_bytes()==reference.read_bytes(),
              historical_semantic_sha256=semantic,exporter_sha256=exporter,
              current_report_sha256=hashlib.sha256(current.read_bytes()).hexdigest(),
              reference_report_sha256=hashlib.sha256(reference.read_bytes()).hexdigest(),
              runtime_approved=False)
current.with_name('frozen-policy-control.json').write_text(json.dumps(evidence,indent=2)+'\n')
print('Same-toolchain frozen report byte equality and historical semantic anchor: PASS')
print(json.dumps(evidence,indent=2))
PY
