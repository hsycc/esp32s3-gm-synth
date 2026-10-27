#!/usr/bin/env python3
"""Compare bounded C++ analysis with frozen v3; no runtime/audio approval."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import random
import struct
import subprocess

from .m7_loop_seam_audio import chunks
from .v1_loop_policy import analyze, POLICY_VERSION
from .v1_loop_review import validated_rows

REASONS = ('DIRECT_SEAM_BELOW_REVIEW_FLOOR', 'ABSOLUTE_IMPROVEMENT_TOO_SMALL',
           'RELATIVE_IMPROVEMENT_TOO_SMALL', 'PERIOD_CHANGE_GE_REVIEW_LIMIT',
           'SPECTRAL_UNDEFINED', 'HIGH_LOCAL_SPECTRAL_DISTANCE', 'LAG_UNDEFINED',
           'LOCAL_LAG_SHIFT_REQUIRES_REVIEW', 'NEAR_SEAM_PEAK_WORSE',
           'NEAR_SEAM_RMS_WORSE', 'FADE_TOO_LONG_FOR_LOOP')
CONTEXT = dict(scope='prepared_zone', loop_mode='continuous', sample_type=1, noteon_address_modulated=False)


def compare(actual, expected):
    if (actual['selected'] != expected['selected_fade_frames'] or
            actual['best'] != expected['seam_best_fade_frames'] or actual['runtime_approved'] is not False):
        raise ValueError('decision differs from frozen v3')
    indexed = {c['fade']: c for c in actual['candidates']}
    if len(indexed) != 4 or set(indexed) != {0, 16, 32, 64}:
        raise ValueError('missing/duplicate candidate')
    for b in expected['candidates']:
        a = indexed[b['fade_frames']]
        mask = sum(1 << REASONS.index(s) for s in b['rejection_reasons'])
        if a['rejected'] != mask:
            raise ValueError(f'rejection difference fade={a["fade"]}: {a["rejected"]} vs {mask}')
        for key, source in (('seam','seam_step'), ('near_peak','near_max'),
                            ('spectral_distance','spectral_distance'), ('lag_shift','lag_shift')):
            if a[key] != b[source]:
                raise ValueError(f'{key} mismatch fade={a["fade"]}: {a[key]} vs {b[source]}')
        if not math.isfinite(a['near_rms']) or abs(a['near_rms']-b['near_rms']) > 1e-8:
            raise ValueError('neighborhood RMS differs')
        for key in ('changed_peak', 'unchanged_peak', 'changed_edges', 'unchanged_edges'):
            other = key.replace('_peak', '_edge_peak')
            if a[key] != b['source_matched_peak'][other]:
                raise ValueError('source-matched edge attribution differs')
    for b in expected['excluded_candidates']:
        if indexed[b['fade_frames']]['rejected'] != 1 << 10:
            raise ValueError('short-loop exclusion differs')
    if not 0 < actual['work'] <= 50000 or not 0 < actual['reads'] <= 2000:
        raise ValueError('bounded work/read contract exceeded')


def synthetic():
    rng = random.Random(725)
    for n in (4,33,34,35,64,65,66,67,129,130,131,192,193,194,1600,1601,3200,3201,6400,6401,7943,65536):
        yield [0]*n
        yield [20000]*n
        yield [(-32768 if i % 2 else 32767) for i in range(n)]
        yield [round(12000*math.sin(2*math.pi*i/31)+2000*math.sin(2*math.pi*i/9)) for i in range(n)]
        yield [rng.randint(-32768,32767) for _ in range(n)]
    for n, period in ((4096,137),(7943,97),(8192,137)):
        yield [round(16000*math.sin(2*math.pi*i/period)) for i in range(n)]
    for _ in range(40):
        n = rng.randrange(132,8193)
        yield [rng.randint(-32768,32767) for _ in range(n)]


def check(probe, sf2=None, report=None):
    data, references, keys = bytearray(), [], []
    if sf2 is None:
        for i,pcm in enumerate(synthetic()):
            data.extend(struct.pack('<I',len(pcm)) + struct.pack('<'+'h'*len(pcm),*pcm))
            references.append(analyze(pcm,CONTEXT)); keys.append(f'synthetic-{i}')
        asset = None
    else:
        contents = json.loads(report.read_bytes())
        indexed = validated_rows(contents)
        asset = hashlib.sha256(sf2.read_bytes()).hexdigest()
        if (asset != contents['asset_sha256'] or contents['policy_version'] != POLICY_VERSION or
                contents['decision_counts'].get('NOT_SAMPLED',0)):
            raise ValueError('complete frozen policy/asset identity required')
        (offset,size),_ = chunks(sf2)
        with sf2.open('rb') as f:
            for key,row in sorted(indexed.items()):
                if not row['candidates']:
                    continue
                g=row['geometry']; start,end=g['loop_start'],g['loop_end']
                if not 0 <= start < end <= size//2 or not 4 <= end-start <= 200000:
                    raise ValueError('out-of-range prepared loop')
                f.seek(offset+2*start);raw=f.read(2*(end-start))
                if len(raw)!=2*(end-start):raise ValueError('truncated PCM')
                data.extend(struct.pack('<I',end-start)+raw);references.append(row);keys.append(key)
        if len(references)!=contents['analyzed_geometries']:
            raise ValueError('incomplete prepared geometry coverage')
    result=subprocess.run([str(probe)],input=data,capture_output=True,timeout=180,check=True)
    rows=[json.loads(line) for line in result.stdout.splitlines()]
    if len(rows)!=len(references) or not rows:raise ValueError('incomplete native probe output')
    for key,actual,expected in zip(keys,rows,references):
        try: compare(actual,expected)
        except ValueError as exc: raise ValueError(f'{key}: {exc}') from exc
        actual['case']=key
    if sf2 and hashlib.sha256(sf2.read_bytes()).hexdigest()!=asset:raise ValueError('asset changed')
    return dict(schema=1,policy_version=POLICY_VERSION,scope='bounded analyzer vs frozen Host v3',
                runtime_approved=False,asset_sha256=asset,cases=len(rows),all_equal=True,
                nonzero=sum(r['selected']!=0 for r in rows),max_work=max(r['work'] for r in rows),
                max_pcm_reads=max(r['reads'] for r in rows),rows=rows)


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--probe',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    p.add_argument('--sf2',type=Path);p.add_argument('--report',type=Path)
    a=p.parse_args()
    if bool(a.sf2)!=bool(a.report):p.error('sf2 and report must be supplied together')
    if a.output.suffix!='.json' or a.output.resolve() in {x.resolve() for x in (a.probe,a.sf2,a.report) if x}:
        p.error('output must be a distinct JSON file')
    r=check(a.probe.resolve(),a.sf2,a.report)
    a.output.parent.mkdir(parents=True,exist_ok=True)
    a.output.write_text(json.dumps(r,indent=2,allow_nan=False)+'\n')
    print(json.dumps({k:v for k,v in r.items() if k!='rows'},indent=2))


if __name__=='__main__':main()
