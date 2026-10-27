#!/usr/bin/env python3
"""Strict native/QEMU evidence for Prepare-time resident analysis (no approval)."""
import argparse
import json
from pathlib import Path
import re

RESULT = re.compile(r'^ANALYSIS_RESULT suites=(\d+) frames=(\d+) trace=(\d+) initial_fade=(\d+) replacement_fade=(\d+) work=(\d+) reads=(\d+)$', re.M)
LAYOUT = re.compile(r'^ANALYSIS_LAYOUT pointer=(\d+) analyzer=(\d+) provider=(\d+) fixture=(\d+)$', re.M)
FATAL = re.compile(r'ANALYSIS_FAIL|CORE_FAIL|Guru Meditation|assert(?:ion)? failed|Assertion .*failed|abort\(\)|panic.ed|task_wdt:', re.I)
EXPECTED = ('6','1024','5270344609859362585','32','0','74708','2896')
DIGEST = '9b8bfcf0226da056839739d65b12cc145aaa2488c26aa753a2ef8a5dea8987fd'


def compare(native,target):
    if FATAL.search(native) or FATAL.search(target):raise ValueError('fatal execution marker')
    a,b=RESULT.findall(native),RESULT.findall(target)
    if a!=[EXPECTED] or b!=a:raise ValueError('missing, duplicate or mismatched application trace')
    for text in (native,target):
        if text.count('ANALYSIS_PASS')!=1 or text.count('ANALYSIS_ASSET sha256='+DIGEST)!=1:
            raise ValueError('missing/duplicate completion or asset identity')
    begin='ANALYSIS_BEGIN esp32s3 macro=1 resident_analysis=1'
    if target.count(begin)!=1 or target.count('ANALYSIS_END')!=1 or target.count('Returned from app_main()')!=1:
        raise ValueError('target entry/return missing or duplicated')
    markers=(begin,'ANALYSIS_RESULT','ANALYSIS_LAYOUT','ANALYSIS_ASSET','ANALYSIS_PASS','ANALYSIS_END','Returned from app_main()')
    indexes=[target.index(s) for s in markers]
    if indexes!=sorted(indexes):raise ValueError('marker order invalid')
    layout=LAYOUT.findall(target)
    if len(layout)!=1 or layout[0][0]!='4' or layout[0][1]!='4096' or any(int(x)<=0 for x in layout[0]):
        raise ValueError('missing/invalid target layout')
    return dict(schema=1,scope='synthetic SF2 -> resident PCM analysis -> joint commit -> MIDI Voice in QEMU',
                runtime_approved=False,hardware_timing_approved=False,
                suites=6,frames=1024,trace=int(EXPECTED[2]),initial_fade=32,replacement_fade=0,
                initial_program_work=74708,initial_program_pcm_reads=2896,asset_sha256=DIGEST,
                target_layout=dict(zip(('pointer','analyzer','provider','fixture'),map(int,layout[0]))))


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('native','target','output'):p.add_argument('--'+name,type=Path,required=True)
    a=p.parse_args();r=compare(a.native.read_text(),a.target.read_text())
    a.output.write_text(json.dumps(r,indent=2)+'\n');print(json.dumps(r,indent=2))


if __name__=='__main__':main()
