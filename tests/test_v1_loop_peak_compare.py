import copy
from collections import Counter
import unittest
from tools.v1_loop_peak_compare import compare
from tools.v1_loop_policy import choose, POLICY_VERSION, LEGACY_POLICY_VERSION
from tests.test_v1_loop_matched_peak import selection_fixture, CTX


def reports():
    pcm = [0]*4096
    pcm[110] = 20000
    metrics = selection_fixture(pcm, 32)
    geometry = dict(sample_id=1, loop_start=0, loop_end=4096,
                    sample_type=1, loop_mode='continuous', noteon_address_modulated=False)
    out = []
    for mode in ('legacy_window', 'source_matched'):
        r = choose(metrics, 4096, CTX, peak_mode=mode)
        r.update(plan_key='a' if mode == 'legacy_window' else 'b', geometry=geometry,
                 references=[dict(sample_id=1, name='synthetic fixture')])
        if mode == 'legacy_window':
            for c in r['candidates']:
                c.pop('source_matched_peak', None)
        out.append(dict(schema=2, runtime_approved=False, rows=[r],
                        policy_version=r['policy_version'], unique_geometries=1,
                        analyzed_geometries=1, decision_counts={r['decision']: 1},
                        asset_sha256='a'*64, exporter_sha256='b'*64, export_summary={}))
    return out


class PeakCompareTests(unittest.TestCase):
    def test_only_misattributed_peak_changes_selection(self):
        result = compare(*reports())
        self.assertEqual(result['changed_selections'], 1)
        self.assertTrue(result['raw_and_nonpeak_metrics_unchanged'])
        self.assertFalse(result['runtime_approved'])

    def test_mutated_raw_metric_or_threshold_is_rejected(self):
        for field in ('seam_step', 'near_rms', 'spectral_distance'):
            a, b = reports()
            b['rows'][0]['candidates'][1][field] += 1
            with self.assertRaises(ValueError):
                compare(a, b)
        a, b = reports()
        b['rows'][0]['policy']['max_period_fraction'] = .05
        with self.assertRaises(ValueError):
            compare(a, b)

    def test_bad_counts_identity_coverage_runtime_rejected(self):
        for mutate in (lambda r: r.update(asset_sha256='c'*64),
                       lambda r: r.update(runtime_approved=True),
                       lambda r: r.update(analyzed_geometries=0),
                       lambda r: r['rows'][0].update(decision='NOT_SAMPLED'),
                       lambda r: r['rows'][0]['geometry'].update(loop_end=4095)):
            a, b = reports()
            b = copy.deepcopy(b)
            mutate(b)
            with self.assertRaises(ValueError):
                compare(a, b)

    def test_nonpeak_rejection_cannot_be_removed(self):
        a, b = reports()
        a['rows'][0]['candidates'][1]['risk_rejections'].append('LOCAL_LAG_SHIFT_REQUIRES_REVIEW')
        with self.assertRaises(ValueError):
            compare(a, b)


if __name__ == '__main__':
    unittest.main()
