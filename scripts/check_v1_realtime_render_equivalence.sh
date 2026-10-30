#!/usr/bin/env bash
# Compare the optimized opt-in renderer with the exact pre-fix application,
# using the original real-bank scenes and the same compiler on both sides.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
BASE=1aeb2cf61df48f180f539ff560fd41d768179903
OUT="${V1_RENDER_EQUIVALENCE_DIR:-build/v1-loop-realtime-render-equivalence}"
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
mkdir -p "$OUT/frozen"
git archive "$BASE" firmware tests/app tests/__init__.py tools | tar -x -C "$OUT/frozen"
python3 "$OUT/frozen/tests/app/make_loop_analysis_fixture.py" "$OUT/generated"
FLAGS=(-std=c++17 -O2 -Wall -Wextra -Werror -pedantic -pthread \
  -Ifirmware -I"$OUT/generated" -DGM_SYNTH_EXPERIMENTAL_VOICE_LOOP=1)
for side in frozen candidate; do
  source_root="$ROOT"
  if [[ "$side" == frozen ]]; then source_root="$OUT/frozen"; fi
  (
    cd "$source_root"
    sources=(firmware/sample/*.cpp firmware/gm/*.cpp firmware/sf2/*.cpp \
      firmware/voice/*.cpp firmware/realtime/*.cpp)
    "${CXX:-g++}" "${FLAGS[@]}" "${sources[@]}" \
      "$OUT/frozen/tests/app/test_loop_output.cpp" -lcrypto -o "$OUT/$side-output"
  )
  "$OUT/$side-output" "$ROOT/assets/sf2/subsets/FM_GM_Open_subset_6MiB.sf2" \
    "$OUT/$side-audition" > "$OUT/$side.log"
done
python3 - "$OUT" "$BASE" "${CXX:-g++}" <<'PY'
import hashlib
import json
from pathlib import Path
import subprocess
import sys

out, reference, compiler = Path(sys.argv[1]), sys.argv[2], sys.argv[3]
rows = []
expected = {
    f'{scene}-loop-{loop}-{mode}.wav'
    for scene in ('fm16', 'music')
    for loop in ('off', 'on')
    for mode in ('bypass', 'peak')
}
observed = set()
for name in ('output-audition.json', 'musical-audition.json'):
    before = json.loads((out / 'frozen-audition' / name).read_text())
    after = json.loads((out / 'candidate-audition' / name).read_text())
    if before != after:
        raise ValueError('real-bank scene, counters or PCM identity changed: ' + name)
    for render in before['renders']:
        filename = render['file']
        if filename in observed or filename not in expected:
            raise ValueError('unexpected or duplicate render: ' + filename)
        observed.add(filename)
        old = (out / 'frozen-audition' / filename).read_bytes()
        new = (out / 'candidate-audition' / filename).read_bytes()
        digest = hashlib.sha256(old).hexdigest()
        if old != new or digest != render['sha256']:
            raise ValueError('render differs from the original complete PCM: ' + filename)
        rows.append(dict(file=filename, bytes=len(old), sha256=digest,
                         byte_identical=True))
if observed != expected:
    raise ValueError('all eight on/off x bypass/peak cohort/music renders required')
report = dict(
    schema=1, scope='same-compiler original-versus-candidate complete Host PCM',
    reference_commit=reference,
    compiler=subprocess.check_output([compiler, '--version'], text=True).splitlines()[0],
    original_scene_and_counters_identical=True, renders=rows,
    physical_execution=False, physical_timing_approved=False,
    listening_approved=False, release_approved=False)
text = json.dumps(report, indent=2) + '\n'
(out / 'render-equivalence.json').write_text(text)
print(text, end='')
PY
