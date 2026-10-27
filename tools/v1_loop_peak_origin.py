#!/usr/bin/env python3
"""Locate whether reported near-wrap maxima are mixed or unchanged PCM edges.

Diagnostic only: does not change thresholds, selections, assets or firmware.
"""
import argparse
import array
import hashlib
import json
from pathlib import Path
import sys


def traced_loop(pcm, fade, frames):
    n = len(pcm)
    if n < 4 or type(fade) is not int or not 0 <= fade < n//2 or frames <= n:
        raise ValueError('invalid loop/fade/output length')
    values, origins, wraps, i = [], [], [], 0
    while len(values) < frames:
        if fade and i >= n-fade:
            j = i-(n-fade)
            v = round((pcm[i]*(fade-j)+pcm[j]*(j+1))/(fade+1))
            values.append(max(-32768, min(32767, v)))
            origins.append(('blend', i, j))
            i += 1
            if i == n:
                i = fade
                wraps.append(len(values))
        else:
            values.append(pcm[i]); origins.append(('source', i))
            i += 1
            if i == n:
                i = 0; wraps.append(len(values))
    return values, origins, wraps


def edge(previous, current):
    if previous[0] == current[0] == 'source' and current[1] == previous[1]+1:
        return (previous[1], current[1])
    return None


def inspect_peak(pcm, fade):
    n = len(pcm); radius = min(96, (n-1)//2); total = n+radius+1
    direct, direct_origins, dw = traced_loop(pcm, 0, total)
    out, origins, wraps = traced_loop(pcm, fade, total)
    positions = range(wraps[0]-radius, wraps[0]+radius+1)
    direct_positions = range(dw[0]-radius, dw[0]+radius+1)
    direct_edges = {edge(direct_origins[i-1], direct_origins[i]) for i in direct_positions}
    direct_edges.discard(None)
    peak = max(abs(out[i]-out[i-1]) for i in positions)
    direct_peak = max(abs(direct[i]-direct[i-1]) for i in direct_positions)
    maxima = []
    for i in positions:
        if abs(out[i]-out[i-1]) != peak:
            continue
        pair = edge(origins[i-1], origins[i])
        maxima.append(dict(output_offset_from_wrap=i-wraps[0],
                           previous_origin=list(origins[i-1]), current_origin=list(origins[i]),
                           unchanged_source_edge=pair is not None,
                           source_edge_in_direct_window=pair in direct_edges))
    return dict(fade_frames=fade, direct_near_peak=direct_peak, candidate_near_peak=peak,
                max_edges_count=len(maxima), max_edges=maxima[:16],
                all_maxima_unchanged_source=all(x['unchanged_source_edge'] for x in maxima),
                all_maxima_absent_from_direct_window=all(not x['source_edge_in_direct_window'] for x in maxima))


def main():
    if __package__:
        from .m7_loop_seam_audio import chunks, render_loop
    else:
        from m7_loop_seam_audio import chunks, render_loop
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('sf2', type=Path)
    p.add_argument('--report', type=Path, required=True)
    p.add_argument('--sample', type=int, action='append', required=True)
    p.add_argument('--output', type=Path, required=True)
    args = p.parse_args()
    if args.output.suffix.lower() != '.json':
        p.error('--output must end in .json')
    if args.output.resolve() in (args.sf2.resolve(), args.report.resolve()):
        p.error('output must not overwrite source evidence')
    report = json.loads(args.report.read_text())
    source_hash = hashlib.sha256(args.sf2.read_bytes()).hexdigest()
    if source_hash != report['asset_sha256'] or report['runtime_approved'] is not False:
        p.error('wrong source bank or runtime-enabled report')
    (offset, pcm_bytes), _ = chunks(args.sf2)
    rows = []
    with args.sf2.open('rb') as f:
        for r in report['rows']:
            g = r['geometry']; start, end = g['loop_start'], g['loop_end']
            if g['sample_id'] not in args.sample or not r['candidates']:
                continue
            if not 0 <= start < end <= pcm_bytes//2 or end-start > 200000:
                raise ValueError('source geometry outside diagnostic bound')
            f.seek(offset+2*start); raw = f.read(2*(end-start))
            if len(raw) != 2*(end-start):
                raise ValueError('truncated PCM')
            data = array.array('h'); data.frombytes(raw)
            if sys.byteorder != 'little': data.byteswap()
            pcm = list(data); diagnostics = []
            for c in r['candidates']:
                fade = c['fade_frames']; total = len(pcm)+min(96, (len(pcm)-1)//2)+1
                values, _, wraps = traced_loop(pcm, fade, total)
                reference, reference_wraps = render_loop(pcm, fade, total)
                if values != reference or wraps != reference_wraps:
                    raise ValueError('trace differs from reference overlap renderer')
                d = inspect_peak(pcm, fade)
                if d['candidate_near_peak'] != c['near_max']:
                    raise ValueError('diagnostic peak does not match prepared report')
                diagnostics.append(d)
            rows.append(dict(plan_key=r['plan_key'], geometry=g, references=r['references'],
                             decision=r['decision'], selected_fade_frames=r['selected_fade_frames'],
                             diagnostics=diagnostics))
    if set(args.sample)-{r['geometry']['sample_id'] for r in rows}:
        raise ValueError('requested sample has no analyzed geometry')
    if hashlib.sha256(args.sf2.read_bytes()).hexdigest() != source_hash:
        raise ValueError('source changed during diagnostics')
    result = dict(scope='peak origin only; no policy or runtime change', asset_sha256=source_hash,
                  runtime_approved=False, reference_renderer_equivalence=True, rows=rows)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2, allow_nan=False)+'\n')
    print(json.dumps([dict(sample_id=r['geometry']['sample_id'], loop_frames=r['geometry']['loop_end']-r['geometry']['loop_start'],
                           diagnostics=r['diagnostics']) for r in rows], indent=2))


if __name__ == '__main__':
    main()
