#!/usr/bin/env python3
"""Controlled v2/v3 evidence comparison; never approves firmware or listening."""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path

if __package__:
    from .v1_loop_policy import choose, Policy, POLICY_VERSION, LEGACY_POLICY_VERSION
else:
    from v1_loop_policy import choose, Policy, POLICY_VERSION, LEGACY_POLICY_VERSION

ANALYZED = {'DIRECT_NO_BENEFIT', 'DIRECT_REVIEW', 'HOST_CANDIDATE'}
DERIVED = {'benefit_rejections', 'risk_rejections', 'context_rejections',
           'rejection_reasons', 'eligible', 'source_matched_peak'}
PEAK = 'NEAR_SEAM_PEAK_WORSE'


def inventory(report):
    if report.get('schema') != 2 or report.get('runtime_approved') is not False:
        raise ValueError('expected disabled schema-2 report')
    out = {}
    for r in report['rows']:
        if r['runtime_fade_frames'] != 0 or r['runtime_approved'] is not False:
            raise ValueError('runtime enablement in review evidence')
        if r['decision'] not in ANALYZED | {'DIRECT_CONTEXT_BLOCKED'}:
            raise ValueError('comparison requires complete coverage')
        k = json.dumps(r['geometry'], sort_keys=True, separators=(',', ':'))
        if k in out:
            raise ValueError('duplicate effective geometry')
        out[k] = r
    counts = dict(Counter(r['decision'] for r in out.values()))
    if (counts != report['decision_counts'] or len(out) != report['unique_geometries'] or
            sum(counts.get(k, 0) for k in ANALYZED) != report['analyzed_geometries']):
        raise ValueError('inconsistent geometry counts')
    return out


def compare(before, after):
    a, b = inventory(before), inventory(after)
    if before['policy_version'] != LEGACY_POLICY_VERSION or after['policy_version'] != POLICY_VERSION:
        raise ValueError('wrong comparison policy versions')
    for field in ('asset_sha256', 'exporter_sha256', 'export_summary'):
        if before[field] != after[field]:
            raise ValueError('source/exporter mismatch: ' + field)
    if set(a) != set(b):
        raise ValueError('effective geometry inventories differ')
    changes, all_removed, references = [], [], []
    checked = 0
    for key in sorted(a):
        old, new = a[key], b[key]
        if old['references'] != new['references'] or old['context_rejections'] != new['context_rejections']:
            raise ValueError('changed Zone references or context')
        g = new['geometry']
        if old['decision'] == 'DIRECT_CONTEXT_BLOCKED':
            if new['decision'] != old['decision'] or new['selected_fade_frames'] != 0 or new['candidates']:
                raise ValueError('context exclusion changed')
            continue
        if new['policy'] != old['policy']:
            raise ValueError('numeric thresholds changed')
        oc = {c['fade_frames']: c for c in old['candidates']}
        nc = {c['fade_frames']: c for c in new['candidates']}
        if (len(oc) != len(old['candidates']) or len(nc) != len(new['candidates']) or
                set(oc) != set(nc)):
            raise ValueError('candidate inventory changed')
        removed = []
        for f, c in oc.items():
            n = nc[f]
            if {k: v for k, v in c.items() if k not in DERIVED} != {k: v for k, v in n.items() if k not in DERIVED}:
                raise ValueError('raw metrics changed')
            if (c['benefit_rejections'] != n['benefit_rejections'] or
                    c['context_rejections'] != n['context_rejections']):
                raise ValueError('non-peak benefit/context changed')
            x, y = set(c['risk_rejections']), set(n['risk_rejections'])
            if x - {PEAK} != y - {PEAK} or y - x:
                raise ValueError('risk changes are not solely removal of misattributed peak')
            if PEAK in x - y:
                removed.append(dict(fade_frames=f, evidence=n['source_matched_peak']))
        context = dict(g, scope='prepared_zone')
        frames = g['loop_end'] - g['loop_start']
        for mode, target in (('legacy_window', old), ('source_matched', new)):
            check = choose(new['candidates'], frames, context, Policy(**new['policy']), peak_mode=mode)
            for field in ('decision', 'selected_fade_frames', 'seam_best_fade_frames'):
                if check[field] != target[field]:
                    raise ValueError('selection is inconsistent: ' + mode)
            for c in check['candidates']:
                target_c = (oc if mode == 'legacy_window' else nc)[c['fade_frames']]
                if c['rejection_reasons'] != target_c['rejection_reasons'] or c['eligible'] != target_c['eligible']:
                    raise ValueError('candidate eligibility inconsistent')
        checked += 1
        detail = dict(geometry=g, reference=new['references'][0],
                      before_decision=old['decision'], before_fade=old['selected_fade_frames'],
                      after_decision=new['decision'], after_fade=new['selected_fade_frames'],
                      removed_peak_rejections=removed)
        if removed:
            all_removed.append(detail)
        if (old['decision'], old['selected_fade_frames']) != (new['decision'], new['selected_fade_frames']):
            if not removed:
                raise ValueError('changed selection without a peak-attribution cause')
            changes.append(detail)
        if g['sample_id'] in (604, 602, 62, 497, 806):
            # Evidence display only. Sample IDs never enter policy selection.
            references.append(dict(detail, candidates=new['candidates']))
    return dict(schema=1, scope='controlled source-matched peak comparison; Host only',
                runtime_approved=False, asset_sha256=after['asset_sha256'],
                exporter_sha256=after['exporter_sha256'],
                before_policy=before['policy_version'], after_policy=after['policy_version'],
                before_counts=before['decision_counts'], after_counts=after['decision_counts'],
                checked_analyzed_geometries=checked, raw_and_nonpeak_metrics_unchanged=True,
                changed_selections=len(changes), changes=changes,
                geometries_with_peak_rejections_removed=len(all_removed),
                peak_rejection_changes=all_removed, references=references)


def markdown(r):
    lines = ['# Source-matched peak policy comparison', '',
             '**Host candidates only. No firmware enablement or audible PASS.**', '',
             f'Asset: `{r["asset_sha256"]}`',
             f'Before: `{r["before_policy"]}` {r["before_counts"]}',
             f'After: `{r["after_policy"]}` {r["after_counts"]}',
             f'Identical geometry/metrics checked: {r["checked_analyzed_geometries"]}',
             f'Changed selections: {r["changed_selections"]}', '',
             '| Sample | Loop frames | Before | After | Attribution removals |',
             '|---:|---:|---|---|---|']
    for c in r['changes']:
        g = c['geometry']
        lines.append(f'| {g["sample_id"]} | {g["loop_end"]-g["loop_start"]} | '
                     f'{c["before_decision"]}/{c["before_fade"]}f | '
                     f'{c["after_decision"]}/{c["after_fade"]}f | '
                     f'{[x["fade_frames"] for x in c["removed_peak_rejections"]]} |')
    lines += ['', 'All changed/mixed edges retain the original Direct peak limit.',
              'Unchanged source pairs are matched exactly to themselves. RMS/period/spectrum/lag gates are unchanged.',
              'RMS still uses fixed output windows. Local lag is not musical pitch.', '']
    return '\n'.join(lines)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--before', required=True, type=Path)
    p.add_argument('--after', required=True, type=Path)
    p.add_argument('--output', required=True, type=Path)
    args = p.parse_args()
    if args.output.suffix.lower() != '.json':
        p.error('--output must end in .json')
    if any(o.resolve() == i.resolve() for o in (args.output, args.output.with_suffix('.md'))
           for i in (args.before, args.after)):
        p.error('output must not overwrite inputs')
    try:
        data = [x.read_bytes() for x in (args.before, args.after)]
        result = compare(*(json.loads(x) for x in data))
        result['input_sha256'] = dict(zip(('before', 'after'),
                                          (hashlib.sha256(x).hexdigest() for x in data)))
    except (OSError, ValueError, KeyError, TypeError) as exc:
        p.exit(2, f'peak comparison failed: {exc}\n')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2, allow_nan=False)+'\n')
    args.output.with_suffix('.md').write_text(markdown(result))
    print(json.dumps({k: v for k, v in result.items()
                      if k not in ('changes', 'references', 'peak_rejection_changes')}, indent=2))


if __name__ == '__main__':
    main()
