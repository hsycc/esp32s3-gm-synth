import copy
from collections import Counter
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from tools.v1_loop_review import (audit, markdown, review_category, validated_rows,
                                 ANALYZED, PERIOD, LAG, PEAK, RMS)


def candidate(fade, reasons=()):
    return dict(fade_frames=fade, seam_step=10000 if not fade else 100,
                period_change_fraction=0 if not fade else .001,
                spectral_distance=None if not fade else .1,
                lag_shift=None if not fade else 0, near_max=10000, near_rms=1000,
                benefit_rejections=[], risk_rejections=list(reasons),
                context_rejections=[], rejection_reasons=list(reasons), eligible=not reasons)


def row(key='a', sid=1, decision='HOST_CANDIDATE'):
    geometry = dict(sample_id=sid, start=0, end=10000, loop_start=100,
                    loop_end=9000, loop_mode='continuous')
    ref = dict(geometry, name='Fixture', bank=0, program=0, zone_index=0)
    analyzed = decision in ANALYZED
    choice = 16 if decision == 'HOST_CANDIDATE' else None if decision == 'NOT_SAMPLED' else 0
    return dict(plan_key=key, geometry=geometry, references=[ref], decision=decision,
                seam_best_fade_frames=16 if analyzed else None,
                selected_fade_frames=choice, runtime_approved=False, runtime_fade_frames=0,
                context_rejections=[], candidates=[candidate(0), candidate(16)] if analyzed else [])


def report(rows):
    return dict(schema=2, runtime_approved=False, rows=rows,
                unique_geometries=len(rows), analyzed_geometries=sum(r['decision'] in ANALYZED for r in rows),
                decision_counts=dict(Counter(r['decision'] for r in rows)),
                asset_sha256='a'*64, exporter_sha256='b'*64,
                policy_version='fixture-v1', export_summary={'zones': len(rows)})


class LoopReviewTests(unittest.TestCase):
    def pair(self):
        a = row()
        return report([a, row('b', 2, 'NOT_SAMPLED')]), report([copy.deepcopy(a), row('b', 2)])

    def test_preserves_sampled_results_and_completes_coverage(self):
        sampled, full = self.pair()
        out = audit(sampled, full, [1, 2], True)
        self.assertEqual(out['preserved_analyzed_geometries'], 1)
        self.assertEqual(len(out['reference_coverage']), 2)
        self.assertFalse(out['runtime_approved'])

    def test_rejects_changed_existing_candidate(self):
        sampled, full = self.pair()
        full['rows'][0]['candidates'][1]['seam_step'] = 99
        with self.assertRaises(ValueError):
            audit(sampled, full)

    def test_rejects_source_or_policy_mismatch(self):
        for field in ('asset_sha256', 'exporter_sha256', 'policy_version', 'export_summary'):
            sampled, full = self.pair()
            full[field] = 'different'
            with self.assertRaises(ValueError):
                audit(sampled, full)

    def test_rejects_incomplete_full_and_missing_reference(self):
        sampled, full = self.pair()
        with self.assertRaises(ValueError):
            audit(sampled, sampled, require_complete=True)
        with self.assertRaises(ValueError):
            audit(sampled, full, [999])
        with self.assertRaises(ValueError):
            audit(sampled, sampled, [2])

    def test_rejects_duplicate_identity_and_bad_counts(self):
        a = row()
        with self.assertRaises(ValueError):
            validated_rows(report([a, copy.deepcopy(a)]))
        r = report([a]); r['analyzed_geometries'] = 2
        with self.assertRaises(ValueError):
            validated_rows(r)

    def test_rejects_runtime_enablement(self):
        for global_flag in (True, False):
            r = report([row()])
            if global_flag:
                r['runtime_approved'] = True
            else:
                r['rows'][0]['runtime_fade_frames'] = 16
            with self.assertRaises(ValueError):
                validated_rows(r)

    def test_rejects_risky_host_selection(self):
        r = row(); r['candidates'][1] = candidate(16, [LAG])
        with self.assertRaises(ValueError):
            validated_rows(report([r]))

    def test_mutually_exclusive_risk_categories(self):
        r = row(decision='DIRECT_REVIEW')
        cases = [([PERIOD, LAG], 'ALL_BENEFIT_ALTERNATIVES_PERIOD_BLOCKED'),
                 ([LAG], 'LAG_ONLY_ALTERNATIVE'), ([PEAK], 'PEAK_ONLY_ALTERNATIVE'),
                 ([RMS], 'RMS_ONLY_ALTERNATIVE'), ([LAG, PEAK], 'COMBINED_RISKS')]
        for reasons, expected in cases:
            r['candidates'][1] = candidate(16, reasons)
            self.assertEqual(review_category(r), expected)
        r['candidates'] = [candidate(0)]
        with self.assertRaises(ValueError):
            review_category(r)

    def test_same_sample_different_regions_preserved(self):
        a, b = row(), row('b')
        b['geometry']['loop_start'] = 200
        r = report([a, b])
        out = audit(r, r, [1], True)
        self.assertEqual(len(out['reference_coverage'][0]['geometries']), 2)

    def test_markdown_contains_all_alternatives_and_no_approval(self):
        sampled, full = self.pair()
        sampled['rows'][0]['references'][0]['name'] = 'Name|with\nnewline'
        full['rows'][0]['references'][0]['name'] = 'Name|with\nnewline'
        text = markdown(audit(sampled, full, [1, 2]))
        self.assertIn('No listening PASS', text)
        self.assertIn('Name/with newline', text)
        self.assertIn('| 16 | 100 |', text)
        self.assertIn('Explicit reference coverage', text)

    def test_cli_module_and_direct_help(self):
        root = Path(__file__).resolve().parents[1]
        for invocation in (['-m', 'tools.v1_loop_review'], ['tools/v1_loop_review.py']):
            r = subprocess.run([sys.executable, *invocation, '--help'], cwd=root,
                               text=True, capture_output=True, check=True)
            self.assertIn('--require-complete', r.stdout)

    def test_cli_round_trip_and_input_protection(self):
        root = Path(__file__).resolve().parents[1]
        sampled, full = self.pair()
        with tempfile.TemporaryDirectory() as td:
            paths = [Path(td)/name for name in ('sampled.json', 'full.json', 'audit.json')]
            paths[0].write_text(json.dumps(sampled)); paths[1].write_text(json.dumps(full))
            cmd = [sys.executable, '-m', 'tools.v1_loop_review', '--sampled', str(paths[0]),
                   '--full', str(paths[1]), '--sample', '2', '--require-complete', '--output']
            subprocess.run(cmd+[str(paths[2])], cwd=root, check=True, capture_output=True)
            self.assertTrue(paths[2].with_suffix('.md').is_file())
            self.assertFalse(json.loads(paths[2].read_text())['runtime_approved'])
            before = paths[0].read_bytes()
            r = subprocess.run(cmd+[str(paths[0])], cwd=root, capture_output=True)
            self.assertNotEqual(r.returncode, 0)
            self.assertEqual(paths[0].read_bytes(), before)


if __name__ == '__main__':
    unittest.main()
