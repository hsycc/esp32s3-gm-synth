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
OUTPUT_HOOKS={'firmware/realtime/ScheduledAudioBlockRenderer.h':2,
              'firmware/realtime/ScheduledAudioBlockRenderer.cpp':1}
subprocess.run(['git','diff','--exit-code',BASE,'--','firmware/sample','firmware/gm','firmware/sf2','firmware/voice','firmware/realtime',
 'firmware/app/BoundedLoopAnalyzer.h','firmware/app/ResidentLoopProposalProvider.h','firmware/app/LoopProgramRuntime.h',
 'tools/v1_loop_policy.py','tools/v1_loop_crossfade_planner.py','tools/v1_loop_matched_peak.py','sdkconfig*',
 *[':(exclude)'+path for path in [*PATCHES,*OUTPUT_HOOKS]]],check=True)
for path in PATCHES:
    assert restore_atomic_contract(path,Path(path).read_text())==old(path), 'protocol changed: '+path
for path,count in OUTPUT_HOOKS.items():
    restored,found=re.subn(r'^    // BEGIN V1_OUTPUT_FINALIZER\n.*?^    // END V1_OUTPUT_FINALIZER\n',
                          '',Path(path).read_text(),flags=re.S|re.M)
    assert found==count and restored==old(path), 'non-additive output modification: '+path
print('Default entry, SDK defaults, original DSP/transactions/scorer and removable output hooks: exact source PASS')
