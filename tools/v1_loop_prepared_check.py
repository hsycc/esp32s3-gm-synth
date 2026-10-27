#!/usr/bin/env python3
"""Production prepare/residency/handoff integration, still no firmware opt-in."""
import argparse
import array
import hashlib
import json
from pathlib import Path
import subprocess
import sys

from .m7_loop_seam_audio import chunks
from .v1_loop_review import validated_rows
from .v1_loop_release_check import Q, pack, held_phase, point, nearest


def reference(pcm, ls, le, fade, step, total, off, release, mode):
    """Full effective-region oracle, including attack and real raw tail."""
    n = len(pcm)
    phase_at_off = held_phase(off, step, ls, le, fade) if off < total else 0
    started = False
    duration = 0
    if mode == 3 and off < total and release and fade:
        remaining = (n*Q-phase_at_off+step-1)//step
        started = point(pcm,ls,le,fade,phase_at_off,True) != point(pcm,ls,le,fade,phase_at_off,False)
        duration = min(release,remaining) if started else 0
    output = []
    for t in range(total):
        hp = held_phase(t,step,ls,le,fade)
        if mode == 1 or t < off:
            output.append(point(pcm,ls,le,fade,hp,True))
            continue
        rp = phase_at_off+(t-off)*step
        if rp >= n*Q:
            break
        raw = point(pcm,ls,le,fade,rp,False)
        if started and duration >= 2 and t-off < duration:
            h = point(pcm,ls,le,fade,hp,True)
            j = t-off
            output.append(nearest(h*(duration-1-j)+raw*j,duration-1))
        else:
            output.append(raw)
    return pack(output)


def check(sf2, report_path, probe):
    payload = report_path.read_bytes()
    report = json.loads(payload)
    indexed = validated_rows(report)
    if report.get('policy_version') != 'prepared-loop-review-v3-source-matched-peak' or report['decision_counts'].get('NOT_SAMPLED',0):
        raise ValueError('complete frozen v3 report required')
    digest = hashlib.sha256(sf2.read_bytes()).hexdigest()
    if digest != report['asset_sha256']:
        raise ValueError('asset/report mismatch')
    (offset,size), _ = chunks(sf2)
    rows = []
    with sf2.open('rb') as f:
        for key,row in sorted(indexed.items()):
            g = row['geometry']
            if not row['candidates'] or (row['decision'] != 'HOST_CANDIDATE' and g['sample_id'] not in (604,602,62)):
                continue
            start,end = g['start'],g['end']
            if not 0 <= start < end <= size//2 or end-start > 1000000:
                raise ValueError('invalid effective region')
            f.seek(offset+2*start)
            raw=f.read(2*(end-start))
            if len(raw)!=2*(end-start):
                raise ValueError('truncated PCM')
            pcm=array.array('h'); pcm.frombytes(raw)
            if sys.byteorder!='little': pcm.byteswap()
            pcm=list(pcm)
            ls,le=g['loop_start']-start,g['loop_end']-start
            mode={'continuous':1,'until_release':3}[g['loop_mode']]
            fade=row['selected_fade_frames']
            zone=row['references'][0]
            cases=[]
            for fade_used,step,block,release in ((0,Q,64,0),(fade,Q,1,0),(fade,Q,64,0),
                                                (fade,Q//2,17,32 if fade else 0),
                                                (fade,5*Q//4,65,32 if fade else 0)):
                total=(le+2*(le-ls-fade_used))*Q//step+31
                off=max(0,((le-max(1,fade_used//2))*Q+step-1)//step)
                expected=reference(pcm,ls,le,fade_used,step,total,off,release,mode)
                args=[str(probe),str(sf2),digest,str(zone['bank']),str(zone['program']),str(zone['zone_index']),
                      str(fade_used),str(step),str(total),str(block),str(off),str(release)]
                p=subprocess.run(args,capture_output=True,timeout=90)
                if p.returncode:
                    raise ValueError(f'native prepare failed sample={g["sample_id"]}: {p.stderr.decode(errors="replace")[:1200]}')
                meta=json.loads(p.stderr)
                if p.stdout != expected:
                    raise ValueError(f'prepared PCM mismatch sample={g["sample_id"]} zone={zone} fade={fade_used}')
                for field in ('sample_id','start','end','loop_start','loop_end','rate'):
                    if meta[field]!=g[field]:
                        raise ValueError('prepared context mismatch: '+field)
                if (meta['mode']!=mode or meta['frames']!=len(expected)//2 or meta['live_refs'] or
                        not meta['retired_before_render'] or not meta['reclaimed'] or not meta['sample_generation']):
                    raise ValueError('invalid lifecycle evidence')
                cases.append(dict(fade_frames=fade_used,step_q32=step,block_frames=block,noteoff_frame=off,
                                  release_frames=release,compared_frames=len(expected)//2,
                                  pcm_sha256=hashlib.sha256(p.stdout).hexdigest(),native=meta,equal=True))
            rows.append(dict(plan_key=key,sample_id=g['sample_id'],decision=row['decision'],reference=zone,cases=cases))
    candidates=sum(r['decision']=='HOST_CANDIDATE' for r in rows)
    if candidates!=report['decision_counts']['HOST_CANDIDATE'] or candidates==0:
        raise ValueError('incomplete candidate coverage')
    if {604,602,62}-{r['sample_id'] for r in rows}:
        raise ValueError('missing reference coverage')
    if hashlib.sha256(sf2.read_bytes()).hexdigest()!=digest:
        raise ValueError('asset changed during verification')
    return dict(schema=1,scope='actual production Prepare/Source/Residency and experimental SPSC sidecar; no Voice integration',
                runtime_approved=False,asset_sha256=digest,report_sha256=hashlib.sha256(payload).hexdigest(),
                probe_sha256=hashlib.sha256(probe.read_bytes()).hexdigest(),host_candidates_checked=candidates,
                geometries_checked=len(rows),cases=sum(len(r['cases']) for r in rows),
                compared_frames=sum(c['compared_frames'] for r in rows for c in r['cases']),
                maximum_host_arena_bytes=max(c['native']['host_arena_bytes'] for r in rows for c in r['cases']),
                all_equal=True,rows=rows)


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('sf2',type=Path); p.add_argument('--report',type=Path,required=True)
    p.add_argument('--probe',type=Path,required=True); p.add_argument('--output',type=Path,required=True)
    a=p.parse_args()
    if a.output.suffix!='.json' or a.output.resolve() in (a.sf2.resolve(),a.report.resolve(),a.probe.resolve()):
        p.error('output must be a distinct .json')
    try: result=check(a.sf2,a.report,a.probe.resolve())
    except (OSError,ValueError,KeyError,TypeError,subprocess.SubprocessError) as e:
        p.exit(2,f'prepared integration failed: {e}\n')
    a.output.parent.mkdir(parents=True,exist_ok=True)
    a.output.write_text(json.dumps(result,indent=2,allow_nan=False)+'\n',encoding='utf-8')
    print(json.dumps({k:v for k,v in result.items() if k!='rows'},indent=2))


if __name__=='__main__': main()
