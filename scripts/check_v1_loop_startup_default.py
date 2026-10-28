#!/usr/bin/env python3
"""Guard the exact old default entry, SDK configs, frozen DSP and scoring."""
from pathlib import Path
import re
import subprocess
from v1_atomic_contract_source import PATCHES, restore_atomic_contract
BASE='d72dcbbd942b34106d6a59496f9e2dafe533e104'
def old(path):return subprocess.check_output(['git','show',f'{BASE}:{path}']).decode()
assert Path('main/app_main.cpp').read_text()==old('main/app_main.cpp'),'default entry changed'
s=Path('main/CMakeLists.txt').read_text();assert s.endswith(old('main/CMakeLists.txt'))
assert 'option(GM_SYNTH_BUILD_LOOP_STARTUP_QUAL "Build isolated startup lifecycle qualification" OFF)' in Path('CMakeLists.txt').read_text()
subprocess.run(['git','diff','--exit-code',BASE,'--','firmware/sample','firmware/gm','firmware/sf2','firmware/voice','firmware/realtime',
 'firmware/app/BoundedLoopAnalyzer.h','firmware/app/ResidentLoopProposalProvider.h','firmware/app/LoopProgramRuntime.h',
 'tools/v1_loop_policy.py','tools/v1_loop_crossfade_planner.py','tools/v1_loop_matched_peak.py','sdkconfig*',
 *[':(exclude)'+path for path in PATCHES]],check=True)
for path in PATCHES:
    assert restore_atomic_contract(path,Path(path).read_text())==old(path), 'protocol changed: '+path
print('Default entry, SDK defaults, frozen DSP/transactions/scorer: exact source PASS')
