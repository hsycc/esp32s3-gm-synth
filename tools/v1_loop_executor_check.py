#!/usr/bin/env python3
"""Compare experimental C++ cursor with offline PCM; never enable runtime plans."""
import argparse
import array
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile

Q = 1 << 32


def pcm_bytes(pcm):
    data = array.array('h', pcm)
    if sys.byteorder != 'little':
        data.byteswap()
    return data.tobytes()


def reference(pcm, fade, step, total, off=None):
    """Offline virtual buffer oracle; does not use C++ arithmetic or state."""
    n = len(pcm)
    virtual = list(pcm)
    if fade:
        virtual[-fade:] = [round((pcm[n-fade+k]*(fade-k)+pcm[k]*(k+1))/(fade+1))
                           for k in range(fade)]
    phase = 0
    output = []
    for frame in range(total):
        if phase >= n*Q:
            break
        loop = off is None or frame < off
        data = virtual if loop else pcm
        i, fraction = divmod(phase, Q)
        j = (fade if loop else n-1) if i+1 == n else i+1
        output.append(data[i] + ((data[j]-data[i])*fraction)//Q)
        phase += step
        if loop and phase >= n*Q:
            phase = fade*Q + (phase-n*Q) % ((n-fade)*Q)
    return pcm_bytes(output)


def probe(binary, pcm_path, fade, step, total, block, off=None):
    result = subprocess.run([str(binary), str(pcm_path), str(fade), str(step), str(total),
                             str(block), str((1 << 64)-1 if off is None else off),
                             '1' if off is None else '3'],
                            check=True, capture_output=True, timeout=60)
    if len(result.stdout) % 2:
        raise ValueError('probe emitted odd PCM byte count')
    return result.stdout


def check_geometry(binary, pcm, fade):
    n = len(pcm)
    total = n + 2*(n-fade) + 19
    rows = []
    configurations = [(0,Q,64,None), (fade,Q,1,None), (fade,Q,64,None),
                      (fade,Q//2,17,None), (fade,5*Q//4,65,None),
                      (fade,Q,64,n-max(1,fade//2))]
    with tempfile.TemporaryDirectory() as td:
        path = Path(td)/'source.pcm'
        path.write_bytes(pcm_bytes(pcm))
        for f, step, block, off in configurations:
            expected = reference(pcm,f,step,total,off)
            actual = probe(binary,path,f,step,total,block,off)
            if expected != actual:
                raise ValueError(f'C++ PCM mismatch: fade={f}, step={step}, block={block}, off={off}')
            rows.append(dict(fade_frames=f, step_q32=step, block_frames=block,
                             noteoff_frame=off, compared_frames=len(actual)//2,
                             pcm_sha256=hashlib.sha256(actual).hexdigest(), equal=True))
    return rows


def check_bank(sf2, report_path, binary, references):
    if __package__:
        from .m7_loop_seam_audio import chunks, render_loop
        from .v1_loop_review import validated_rows
    else:
        from m7_loop_seam_audio import chunks, render_loop
        from v1_loop_review import validated_rows
    payload = report_path.read_bytes()
    report = json.loads(payload)
    indexed = validated_rows(report)
    if report['decision_counts'].get('NOT_SAMPLED',0):
        raise ValueError('complete prepared geometry report required')
    if report['policy_version'] != 'prepared-loop-review-v3-source-matched-peak':
        raise ValueError('expected frozen v3 policy')
    asset_hash = hashlib.sha256(sf2.read_bytes()).hexdigest()
    if asset_hash != report['asset_sha256']:
        raise ValueError('bank does not match report')
    (offset, size), _ = chunks(sf2)
    rows = []
    with sf2.open('rb') as f:
        for key, r in sorted(indexed.items()):
            g = r['geometry']
            if r['decision'] != 'HOST_CANDIDATE' and g['sample_id'] not in references:
                continue
            if not r['candidates']:
                continue
            start, end = g['loop_start'], g['loop_end']
            if not 0 <= start < end <= size//2 or not 4 <= end-start <= 200000:
                raise ValueError('bad effective loop range')
            f.seek(offset+2*start)
            raw = f.read(2*(end-start))
            if len(raw) != 2*(end-start):
                raise ValueError('truncated loop')
            data = array.array('h'); data.frombytes(raw)
            if sys.byteorder != 'little':
                data.byteswap()
            pcm = list(data)
            fade = r['selected_fade_frames']
            # Check the independent oracle against the original audition exporter
            # at unity step, over several wraps, before comparing to C++.
            total = len(pcm)+2*(len(pcm)-fade)+19
            old, _ = render_loop(pcm,fade,total)
            if pcm_bytes(old) != reference(pcm,fade,Q,total):
                raise ValueError('oracle differs from frozen audition renderer')
            cases = check_geometry(binary,pcm,fade)
            rows.append(dict(plan_key=key,sample_id=g['sample_id'],loop_frames=len(pcm),
                             selected_fade_frames=fade,decision=r['decision'],cases=cases))
    expected = report['decision_counts'].get('HOST_CANDIDATE',0)
    if not expected or sum(r['decision']=='HOST_CANDIDATE' for r in rows) != expected:
        raise ValueError('incomplete Host candidate execution coverage')
    if set(references) - {r['sample_id'] for r in rows}:
        raise ValueError('missing explicit reference coverage')
    if hashlib.sha256(sf2.read_bytes()).hexdigest() != asset_hash:
        raise ValueError('source changed during verification')
    return dict(scope='experimental mono cursor equivalence; not Voice integration or audible acceptance',
                runtime_approved=False,asset_sha256=asset_hash,
                report_sha256=hashlib.sha256(payload).hexdigest(),
                probe_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
                policy_version=report['policy_version'],host_candidates_checked=expected,
                geometries_checked=len(rows),cases=sum(len(r['cases']) for r in rows),
                compared_frames=sum(c['compared_frames'] for r in rows for c in r['cases']),
                all_equal=True,rows=rows)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('sf2',type=Path)
    p.add_argument('--report',type=Path,required=True)
    p.add_argument('--probe',type=Path,required=True)
    p.add_argument('--reference',type=int,action='append',default=[])
    p.add_argument('--output',type=Path,required=True)
    a = p.parse_args()
    if a.output.suffix.lower() != '.json' or a.output.resolve() in (a.sf2.resolve(),a.report.resolve(),a.probe.resolve()):
        p.error('output must be a distinct .json file')
    try:
        result=check_bank(a.sf2,a.report,a.probe.resolve(),a.reference)
    except (OSError,ValueError,KeyError,TypeError,subprocess.SubprocessError) as exc:
        p.exit(2,f'cursor verification failed: {exc}\n')
    a.output.parent.mkdir(parents=True,exist_ok=True)
    a.output.write_text(json.dumps(result,indent=2,allow_nan=False)+'\n')
    print(json.dumps({k:v for k,v in result.items() if k!='rows'},indent=2))


if __name__=='__main__':
    main()
