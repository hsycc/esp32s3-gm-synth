#!/usr/bin/env python3
"""Exact frozen-source checks, including removable default-off app hooks."""
from pathlib import Path
import re
import subprocess
from v1_atomic_contract_source import restore_atomic_contract

BASE='6d954a7bc200cd0ec300873e7e106fff2a87a692'
bindings=['firmware/voice/experimental/PinnedLoopPlan.h',
          'firmware/voice/experimental/PreparedLoopHandoff.h']
hooks={'firmware/gm/ZoneResolver.h':3,'firmware/gm/ZoneResolver.cpp':4}
allowed=['firmware/voice/VoiceRenderer.h','firmware/voice/VoiceRenderer.cpp',
         'firmware/voice/VoiceEffectsBus.cpp',*bindings,*hooks]
paths=['firmware/sample','firmware/gm','firmware/sf2','firmware/voice',
       'tools/v1_loop_policy.py','tools/v1_loop_crossfade_planner.py',
       'tools/v1_loop_matched_peak.py','tools/v1_effective_loop_survey.py',
       'tools/m7_loop_seam_audio.py','tools/v1_loop_executor_check.py',
       'tools/v1_loop_release_check.py','tools/v1_loop_prepared_check.py',
       'tools/loop_cursor_probe.cpp','tools/loop_pinned_probe.cpp','tools/loop_prepared_probe.cpp']
subprocess.run(['git','diff','--exit-code',BASE,'--',*paths,
                *[':(exclude)'+p for p in allowed]],check=True)

def remove_app(text):
    return re.subn(r'^[ \t]*// BEGIN V1_APP_LOOP\n.*?^[ \t]*// END V1_APP_LOOP\n',
                   '',text,flags=re.S|re.M)

for path in bindings:
    text=restore_atomic_contract(path, Path(path).read_text())
    original=subprocess.check_output(['git','show',f'{BASE}:{path}']).decode()
    text,app_count=remove_app(text)
    if app_count != (1 if path.endswith('PreparedLoopHandoff.h') else 0):
        raise SystemExit('unexpected app insertion count: '+path)
    trimmed,count=re.subn(r'    // BEGIN V1_VOICE_BINDING\n.*?    // END V1_VOICE_BINDING\n','',text,flags=re.S)
    if trimmed!=original and count==1:
        trimmed=trimmed.replace('\n\n\n    bool reset()', '\n\n    bool reset()',1)
    if count!=1 or trimmed!=original:
        raise SystemExit(f'non-additive modification to frozen baseline: {path}')
for path,expected in hooks.items():
    original=subprocess.check_output(['git','show',f'{BASE}:{path}']).decode()
    trimmed,count=remove_app(Path(path).read_text())
    if count!=expected or trimmed!=original:
        raise SystemExit('non-additive resolver modification: '+path)
print('Frozen policy, transport, directory/prepare and original resolver/handoff bodies: exact PASS')
