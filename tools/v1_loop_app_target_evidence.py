#!/usr/bin/env python3
"""Strict native/target application transaction log checks; no hardware approval."""
import argparse
import hashlib
import json
from pathlib import Path
import re

DIGEST='e1c3af71233c2e6b6151e53dbca19ce7bd05e0f14d160cf3ed11a2da419af51f'
FATAL=re.compile(r'V1_APP_FAIL|Guru Meditation|assert(?:ion)? failed|Assertion .*failed|abort\(\)|panic.ed|task_wdt:|Stack canary watchpoint')
TRACE=re.compile(r'^APP_TARGET_TRACE frames=(\d+) hash=(\d+) events=(\d+) commits=(\d+)$',re.M)
LAYOUT=re.compile(r'^APP_TARGET_LAYOUT pointer=(\d+) fixture=(\d+) workspace=(\d+) channel=(\d+) runtime=(\d+)$',re.M)
IDENTITY='APP_TARGET_IDENTITY PASS checks=19 computed_sha256=1 rebind_rejected=1'
CONCURRENT='APP_TARGET_CONCURRENT polls=500 cycles=32 PASS'
RESULT=f'APP_TARGET_RESULT suites=3 sha256={DIGEST} PASS runtime_approved=0'
BEGIN='APP_TARGET_BEGIN esp32s3 macro=1 embedded_sf2=1'
END='APP_TARGET_END'
# Fixed trace from the reviewed shared script; native and target must both match.
EXPECTED=(1024,3502127162542228268,20,4)

def line_once(text, marker):
    hits=list(re.finditer('^'+re.escape(marker)+'$',text,re.M))
    if len(hits)!=1: raise ValueError('missing/duplicate marker: '+marker)
    return hits[0].start()

def compare(native, target):
    native=native.replace('\r\n','\n');target=target.replace('\r\n','\n')
    parsed=[]
    for text in (native,target):
        if FATAL.search(text): raise ValueError('fatal execution marker')
        trace=TRACE.findall(text);layout=LAYOUT.findall(text)
        if len(trace)!=1 or tuple(map(int,trace[0]))!=EXPECTED: raise ValueError('wrong/missing trace coverage')
        if len(layout)!=1 or any(int(v)<=0 for v in layout[0]): raise ValueError('invalid layout')
        points=[line_once(text,IDENTITY),text.index('APP_TARGET_TRACE'),line_once(text,CONCURRENT),
                text.index('APP_TARGET_LAYOUT'),line_once(text,RESULT)]
        if points!=sorted(points):raise ValueError('marker order')
        parsed.append(dict(trace=list(map(int,trace[0])),layout=list(map(int,layout[0]))))
    if parsed[1]['layout'][0]!=4: raise ValueError('target is not 32-bit')
    begin=line_once(target,BEGIN);end=line_once(target,END)
    if not begin<target.index(IDENTITY)<target.index(RESULT)<end: raise ValueError('target begin/end order')
    returns=list(re.finditer(r'Returned from app_main\(\)',target))
    if len(returns)!=1 or returns[0].start()<end: raise ValueError('missing/duplicate normal app return')
    return dict(schema=1,scope='embedded synthetic SF2 through production application transaction on Xtensa QEMU',
                runtime_approved=False,hardware_timing_approved=False,all_equal=True,suites=3,
                asset_sha256=DIGEST,output_frames=EXPECTED[0],pcm_state_hash=EXPECTED[1],
                scheduled_events=EXPECTED[2],joint_commits=EXPECTED[3],paused_ready_polls=500,
                concurrent_cycles=32,target_layout=dict(zip(('pointer','fixture','workspace','channel','runtime'),parsed[1]['layout'])),
                native_log_sha256=hashlib.sha256(native.encode()).hexdigest(),target_log_sha256=hashlib.sha256(target.encode()).hexdigest())

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('native','target','output'):p.add_argument('--'+name,type=Path,required=True)
    a=p.parse_args()
    try:r=compare(a.native.read_text(),a.target.read_text())
    except (ValueError,OSError) as e:p.exit(2,str(e)+'\n')
    a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_text(json.dumps(r,indent=2)+'\n');print(json.dumps(r,indent=2))
if __name__=='__main__':main()
