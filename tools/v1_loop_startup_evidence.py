#!/usr/bin/env python3
"""Strict startup execution evidence, never a physical timing/audio approval."""
import argparse
import json
from pathlib import Path
import re

DIGEST='9b8bfcf0226da056839739d65b12cc145aaa2488c26aa753a2ef8a5dea8987fd'
FATAL=re.compile(r'STARTUP_FAIL|Guru Meditation|[Aa]ssert(?:ion)? .*failed|abort\(\)|panic.ed|task_wdt:')

def one(pattern,text):
    found=re.findall(pattern,text,re.M)
    if len(found)!=1: raise ValueError('missing or duplicate evidence: '+pattern)
    return found[0]

def validate(native,target,analysis):
    if analysis not in (0,1):raise ValueError('invalid opt in')
    if FATAL.search(native) or FATAL.search(target):raise ValueError('fatal marker')
    suites,checks,on,off,frames=map(int,one(r'^STARTUP_HOST suites=(\d+) checks=(\d+) automatic=(\d+) direct=(\d+) frames=(\d+)$',native))
    if suites!=16 or checks<2000 or frames!=16384 or on==off or native.count('STARTUP_HOST_PASS')!=1:
        raise ValueError('native scope not verified')
    if one(r'^STARTUP_BEGIN profile=(\w+) analysis=(\d+) root_application=1$',target)!=('qemu',str(analysis)):
        raise ValueError('wrong profile')
    ready=tuple(map(int,one(r'^STARTUP_READY status=(\d+) session=(\d+) plans=(\d+) analyzed=(\d+) work=(\d+) reads=(\d+) arena_capacity=(\d+) session_bytes=(\d+)$',target)))
    status,session,plans,analyzed,work,reads,capacity,size=ready
    if status!=1 or session!=1 or capacity!=20480 or size<=0 or (analysis and not(plans>0 and analyzed>0 and work>0 and reads>0)) or (not analysis and any((plans,analyzed,work,reads))):
        raise ValueError('invalid startup admission')
    if one(r'^STARTUP_ASSET bytes=(\d+) sha256=([0-9a-f]{64})$',target)!=('25866',DIGEST):
        raise ValueError('wrong resource')
    output=tuple(map(int,one(r'^STARTUP_RESULT frames=(\d+) trace=(\d+) initial_loops=(\d+) replacement_fade=(\d+) events=(\d+) rejected=(\d+)$',target)))
    if output!=(16384,on if analysis else off,analysis,0,3,0):raise ValueError('MIDI/output mismatch')
    if one(r'^STARTUP_STOP releases=(\d+) mapped=(\d+) arena_live=(\d+)$',target)!=('1','0','0'):
        raise ValueError('resource not released exactly once')
    if target.count('STARTUP_PASS runtime_approved=0')!=1 or target.count('Returned from app_main()')!=1:
        raise ValueError('incomplete or duplicate execution')
    ordered=['STARTUP_BEGIN','STARTUP_READY','STARTUP_RESULT','STARTUP_STOP','STARTUP_PASS','Returned from app_main()']
    if [target.index(x) for x in ordered]!=sorted(target.index(x) for x in ordered):raise ValueError('bad order')
    one(r'^STARTUP_TIMING startup_us=\d+ hash_us=\d+ service_max_us=\d+ render_avg_us=\d+ render_max_us=\d+ diagnostic_only=1$',target)
    pcm=tuple(map(int,one(r'^STARTUP_PCM capacity=(\d+) high_water=(\d+) used=(\d+) allocation_failures=(\d+)$',target)))
    if not(0<=pcm[2]<=pcm[1]<=pcm[0]==20480) or pcm[3]:raise ValueError('PCM budget/failure')
    return dict(schema=1,scope='root application, mapped synthetic flash, owned lifecycle, serialized MIDI exercise',
                runtime_approved=False,physical_timing_approved=False,analysis=analysis,frames=output[0],trace=output[1],
                session_bytes=size,arena_bytes=capacity,pcm_high_water=pcm[1],source_releases=1)

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    for x in ('native','target','output'):p.add_argument('--'+x,type=Path,required=True)
    p.add_argument('--analysis',type=int,choices=(0,1),required=True);a=p.parse_args()
    result=validate(a.native.read_text(),a.target.read_text(),a.analysis)
    a.output.write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))
