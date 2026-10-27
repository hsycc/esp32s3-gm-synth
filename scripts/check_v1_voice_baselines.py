#!/usr/bin/env python3
"""Exact frozen-source check; only five explicitly reviewed integration files vary."""
from pathlib import Path
import re
import subprocess

BASE='6d954a7bc200cd0ec300873e7e106fff2a87a692'
allowed=['firmware/voice/VoiceRenderer.h','firmware/voice/VoiceRenderer.cpp',
         'firmware/voice/VoiceEffectsBus.cpp','firmware/voice/experimental/PinnedLoopPlan.h',
         'firmware/voice/experimental/PreparedLoopHandoff.h']
paths=['firmware/sample','firmware/gm','firmware/sf2','firmware/voice',
       'tools/v1_loop_policy.py','tools/v1_loop_crossfade_planner.py',
       'tools/v1_loop_matched_peak.py','tools/v1_effective_loop_survey.py',
       'tools/m7_loop_seam_audio.py','tools/v1_loop_executor_check.py',
       'tools/v1_loop_release_check.py','tools/v1_loop_prepared_check.py',
       'tools/loop_cursor_probe.cpp','tools/loop_pinned_probe.cpp','tools/loop_prepared_probe.cpp']
subprocess.run(['git','diff','--exit-code',BASE,'--',*paths,
                *[':(exclude)'+p for p in allowed]],check=True)
for path in allowed[-2:]:
    text=Path(path).read_text()
    original=subprocess.check_output(['git','show',f'{BASE}:{path}']).decode()
    # Insertions only: existing ownership and handoff contracts cannot drift.
    trimmed,count=re.subn(r'    // BEGIN V1_VOICE_BINDING\n.*?    // END V1_VOICE_BINDING\n','',text,flags=re.S)
    # The pinned header insertion includes one separator blank line.
    if trimmed!=original and count==1:
        trimmed=trimmed.replace('\n\n\n    bool reset()', '\n\n    bool reset()',1)
    if count!=1 or trimmed!=original:
        raise SystemExit(f'non-additive modification to frozen baseline: {path}')
print('Frozen policy, transport, directory/prepare and pre-existing plan/handoff bodies: exact PASS')
