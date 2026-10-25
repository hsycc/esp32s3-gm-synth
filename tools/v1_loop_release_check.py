#!/usr/bin/env python3
"""Bounded-release execution evidence; no runtime or listening approval.

Actual Zone modes and forced UntilRelease stress cases are labeled separately.
The offline oracle uses closed-form phases and immutable virtual samples; it
never calls the C++ cursor or reuses its state machine.
"""
import argparse
import array
from collections import Counter
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile

Q = 1 << 32
POLICY = 'prepared-loop-review-v3-source-matched-peak'


def pack(pcm):
    data = array.array('h', pcm)
    if sys.byteorder != 'little':
        data.byteswap()
    return data.tobytes()


def nearest(n, d):
    return (n+d//2)//d if n >= 0 else -((-n+d//2)//d)


def held_phase(t, step, ls, le, fade):
    position = t*step
    return position if position < le*Q else (ls+fade)*Q + (position-le*Q) % ((le-ls-fade)*Q)


def point(pcm, ls, le, fade, phase, loop):
    i, fraction = divmod(phase, Q)
    j = ls+fade if loop and i >= ls and i+1 >= le else min(i+1, len(pcm)-1)
    def value(k):
        if loop and fade and le-fade <= k < le:
            offset = k-(le-fade)
            return nearest(pcm[k]*(fade-offset)+pcm[ls+offset]*(offset+1), fade+1)
        return pcm[k]
    a, b = value(i), value(j)
    return a+(b-a)*fraction//Q


def reference(pcm, ls, le, fade, step, off, release, mode=3):
    if (not 0 <= ls < le <= len(pcm) or not 4 <= len(pcm) <= 1000000 or
            fade not in (0,16,32,64) or fade >= (le-ls)//2 or
            not 0 < step < 1 << 64 or not 0 <= off <= 4000000 or
            release not in (0,16,32,64) or mode not in (1,3)):
        raise ValueError('invalid release oracle configuration')
    phase = held_phase(off,step,ls,le,fade)
    remaining = (len(pcm)*Q-phase+step-1)//step
    held = point(pcm,ls,le,fade,phase,True)
    raw = point(pcm,ls,le,fade,phase,False)
    started = bool(mode == 3 and release and fade and held != raw)
    begin, end = max(0,off-4), off+129
    stop = min(end,off+remaining) if mode == 3 else end
    output = []
    for t in range(begin,stop):
        hp = held_phase(t,step,ls,le,fade)
        h = point(pcm,ls,le,fade,hp,True)
        if mode == 1 or t < off:
            output.append(h)
            continue
        rp = phase+(t-off)*step
        raw = point(pcm,ls,le,fade,rp,False)
        if started and t-off < release:
            weight = release-1 if t-off == remaining-1 else t-off
            output.append(nearest(h*(release-1-weight)+raw*weight,release-1))
        else:
            output.append(raw)
    active = mode == 1 or stop < off+remaining
    final_phase = (held_phase(stop,step,ls,le,fade) if mode == 1 else
                   phase+(stop-off)*step if active else len(pcm)*Q)
    meta = dict(started=started,truncated=bool(started and remaining < release),
                bridge_frames=min(release,remaining) if started else 0,
                phase_q32=final_phase,active=active,emitted=len(output))
    return output, meta


def probe(binary, path, ls, le, fade, step, off, release, block, mode):
    run = subprocess.run([str(binary),str(path),str(ls),str(le),str(fade),str(step),
                          str(off),str(release),str(block),str(mode)],
                         check=True,capture_output=True,timeout=60)
    if len(run.stdout)%2:
        raise ValueError('odd native PCM byte count')
    return run.stdout,json.loads(run.stderr)


def cases(le, ls, fade, source_mode):
    midpoint = le-max(1,fade//2)
    configs = [(Q,midpoint,32,source_mode,'actual_zone_mode'),
               (Q,midpoint,0,3,'immediate_control'),
               (Q,midpoint,32,3,'direct_control')]
    for duration in (16,32,64):
        for pos in (midpoint,le-1):
            configs.append((Q,pos,duration,3,'forced_until_release'))
    for step in (Q//2,5*Q//4):
        for target in ((le-fade)*Q-Q//2,(le-1)*Q):
            off = max(0,(target+step-1)//step)
            configs.append((step,off,32,3,'forced_until_release'))
    configs.append((Q,le+(le-ls-fade)-max(1,fade//2),32,3,'second_wrap_release'))
    return configs


def check_geometry(binary, pcm, ls, le, fade, source_mode):
    rows = []
    with tempfile.TemporaryDirectory() as td:
        path = Path(td)/'region.pcm'
        path.write_bytes(pack(pcm))
        for step,off,release,mode,label in cases(le,ls,fade,source_mode):
            chosen = 0 if label == 'direct_control' else fade
            values,expected_meta = reference(pcm,ls,le,chosen,step,off,release,mode)
            expected = pack(values)
            for block in (1,64):
                actual,meta = probe(binary,path,ls,le,chosen,step,off,release,block,mode)
                if actual != expected or meta != expected_meta:
                    raise ValueError(f'release mismatch: {label}, fade={chosen}, step={step}, '
                                     f'off={off}, release={release}, block={block}; {meta} != {expected_meta}')
            immediate,_ = reference(pcm,ls,le,chosen,step,off,0,mode)
            index = min(off,4)
            natural = point(pcm,ls,le,chosen,held_phase(off,step,ls,le,chosen),True)
            switch_before = abs(immediate[index]-natural)
            switch_after = abs(values[index]-natural)
            if meta['started'] and not (meta['truncated'] and meta['bridge_frames']==1) and switch_after:
                raise ValueError('first bridge sample differs from held continuation')
            rows.append(dict(label=label,mode=mode,fade_frames=chosen,release_output_frames=release,
                             step_q32=step,noteoff_frame=off,blocks_checked=[1,64],**meta,
                             switch_mismatch_before=switch_before,switch_mismatch_after=switch_after,
                             onset_step_before=abs(immediate[index]-immediate[index-1]) if index else None,
                             onset_step_after=abs(values[index]-values[index-1]) if index else None,
                             pcm_sha256=hashlib.sha256(expected).hexdigest(),equal=True))
    return rows


def check_bank(sf2, report_path, binary):
    if __package__:
        from .m7_loop_seam_audio import chunks
        from .v1_loop_review import validated_rows
    else:
        from m7_loop_seam_audio import chunks
        from v1_loop_review import validated_rows
    payload = report_path.read_bytes()
    report = json.loads(payload)
    indexed = validated_rows(report)
    if report['policy_version'] != POLICY or report['decision_counts'].get('NOT_SAMPLED',0):
        raise ValueError('complete frozen v3 report required')
    bank_hash = hashlib.sha256(sf2.read_bytes()).hexdigest()
    if bank_hash != report['asset_sha256']:
        raise ValueError('source bank differs from report')
    (offset,size),_ = chunks(sf2)
    rows = []
    with sf2.open('rb') as f:
        for key,r in sorted(indexed.items()):
            g = r['geometry']
            if r['decision'] != 'HOST_CANDIDATE' and g['sample_id'] not in (604,602,62):
                continue
            if not r['candidates']:
                continue
            start,end,ls,le = (g[k] for k in ('start','end','loop_start','loop_end'))
            if not (0 <= start <= ls < le <= end <= size//2 and 4 <= end-start <= 1000000):
                raise ValueError('effective PCM region outside bounds')
            if g['loop_mode'] not in ('continuous','until_release'):
                raise ValueError('loop mode unsupported in release evidence')
            f.seek(offset+2*start)
            raw = f.read(2*(end-start))
            if len(raw) != 2*(end-start):
                raise ValueError('truncated effective PCM')
            pcm = array.array('h'); pcm.frombytes(raw)
            if sys.byteorder != 'little':
                pcm.byteswap()
            mode = 1 if g['loop_mode']=='continuous' else 3
            results = check_geometry(binary,list(pcm),ls-start,le-start,r['selected_fade_frames'],mode)
            rows.append(dict(plan_key=key,geometry=g,decision=r['decision'],
                             selected_fade_frames=r['selected_fade_frames'],cases=results))
    expected = report['decision_counts'].get('HOST_CANDIDATE',0)
    if not expected or sum(r['decision']=='HOST_CANDIDATE' for r in rows) != expected:
        raise ValueError('incomplete candidate coverage')
    if {604,602,62}-{r['geometry']['sample_id'] for r in rows}:
        raise ValueError('missing audition-reference geometry')
    if hashlib.sha256(sf2.read_bytes()).hexdigest() != bank_hash:
        raise ValueError('source changed during release analysis')
    tests = [c for r in rows for c in r['cases']]
    triggered = [c for c in tests if c['started']]
    return dict(schema=1,scope='isolated opt-in release bridge; no Voice integration or audible PASS',
                runtime_approved=False,policy_version=report['policy_version'],asset_sha256=bank_hash,
                report_sha256=hashlib.sha256(payload).hexdigest(),
                probe_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
                candidate_source_modes=dict(Counter(r['geometry']['loop_mode'] for r in rows
                                                    if r['decision']=='HOST_CANDIDATE')),
                host_candidates_checked=expected,geometries_checked=len(rows),
                oracle_cases=len(tests),native_runs=2*len(tests),
                compared_frames=2*sum(c['emitted'] for c in tests),
                bridges_started=len(triggered),truncated_bridges=sum(c['truncated'] for c in triggered),
                onset_continuation_equal=sum(c['switch_mismatch_after']==0 for c in triggered),
                single_frame_tail_fallbacks=sum(c['truncated'] and c['bridge_frames']==1 for c in triggered),
                all_equal=True,limitations=[
                    'Forced UntilRelease cases do not change the original Zone mode.',
                    'Zero switch mismatch is not zero adjacent-sample slope or listening approval.',
                    'Natural tail end/large rate changes can truncate the bridge; never extend lifetime.',
                    'No hardware timing, SampleRef ownership, envelope, sustain or full-FX integration.'],rows=rows)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('sf2',type=Path)
    p.add_argument('--report',required=True,type=Path)
    p.add_argument('--probe',required=True,type=Path)
    p.add_argument('--output',required=True,type=Path)
    a = p.parse_args()
    if a.output.suffix.lower()!='.json' or a.output.resolve() in (a.sf2.resolve(),a.report.resolve(),a.probe.resolve()):
        p.error('output must be a distinct .json file')
    try:
        result = check_bank(a.sf2,a.report,a.probe.resolve())
    except (OSError,ValueError,TypeError,KeyError,subprocess.SubprocessError) as exc:
        p.exit(2,f'release check failed: {exc}\n')
    a.output.parent.mkdir(parents=True,exist_ok=True)
    a.output.write_text(json.dumps(result,indent=2,allow_nan=False)+'\n',encoding='utf-8')
    print(json.dumps({k:v for k,v in result.items() if k!='rows'},indent=2))


if __name__=='__main__':
    main()
