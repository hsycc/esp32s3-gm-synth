import copy
import random
import unittest

from tools.m7_loop_seam_audio import render_loop
from tools.v1_loop_matched_peak import point, matched_peak_metrics, valid_peak_evidence
from tools.v1_loop_policy import analyze, choose, POLICY_VERSION, LEGACY_POLICY_VERSION

CTX = dict(scope="prepared_zone", loop_mode="continuous", sample_type=1,
           noteon_address_modulated=False)


def measure(pcm, fade):
    total = len(pcm) + min(96, (len(pcm) - 1) // 2) + 1
    out, wraps = render_loop(pcm, fade, total)
    return matched_peak_metrics(pcm, fade, out, wraps[0])


def selection_fixture(pcm, fade):
    """Control non-peak metrics to isolate the peak selector, not an audition."""
    d, c = measure(pcm, 0), measure(pcm, fade)
    common = dict(near_rms=1000, spectral_distance=.1, lag_shift=0)
    return [dict(common, fade_frames=0, seam_step=10000,
                 near_max=d['candidate_near_peak'], source_matched_peak=d),
            dict(common, fade_frames=fade, seam_step=100,
                 near_max=c['candidate_near_peak'], source_matched_peak=c)]


class MatchedPeakTests(unittest.TestCase):
    def test_shifted_unchanged_peak_is_not_new(self):
        pcm = [0] * 4096
        pcm[110] = 20000
        m = measure(pcm, 32)
        self.assertEqual(m['reference_near_peak'], 0)
        self.assertEqual(m['candidate_near_peak'], 20000)
        self.assertEqual(m['changed_edge_peak'], 0)
        self.assertEqual(m['shifted_source_edge_peak'], 20000)
        self.assertTrue(valid_peak_evidence(m, 20000, 0, len(pcm)))
        rows = selection_fixture(pcm, 32)
        self.assertEqual(choose(rows, len(pcm), CTX)['decision'], 'DIRECT_REVIEW')
        self.assertEqual(choose(rows, len(pcm), CTX, peak_mode='source_matched')['decision'], 'HOST_CANDIDATE')

    def test_shifted_maximum_does_not_hide_smaller_new_mixed_peak(self):
        pcm = [0] * 4096
        for i in range(32):
            pcm[i] = i * 1000
        pcm[32:110] = [31000] * 78
        pcm[110] = -31000
        m = measure(pcm, 32)
        self.assertEqual(m['reference_near_peak'], 1000)
        self.assertEqual(m['unchanged_edge_peak'], 62000)
        self.assertGreater(m['changed_edge_peak'], 1000)
        self.assertLess(m['changed_edge_peak'], 62000)
        r = choose(selection_fixture(pcm, 32), len(pcm), CTX, peak_mode='source_matched')
        self.assertIn('NEAR_SEAM_PEAK_WORSE', r['candidates'][1]['risk_rejections'])
        self.assertEqual(r['selected_fade_frames'], 0)

    def test_point_mapping_matches_reference_multiwrap_random(self):
        rng = random.Random(73019)
        for n in (4, 10, 42, 129, 257, 1000):
            pcm = [rng.randrange(-32768, 32768) for _ in range(n)]
            for fade in (0, 1, n // 2 - 1):
                total = 3 * n + 7
                out, _ = render_loop(pcm, fade, total)
                self.assertEqual([point(pcm, fade, i)[0] for i in range(total)], out)
                m = measure(pcm, fade)
                self.assertTrue(valid_peak_evidence(m, m['candidate_near_peak'], m['reference_near_peak'], n))

    def test_entry_exit_and_wrap_edges_are_checked(self):
        m = measure(list(range(4096)), 32)
        self.assertEqual(m['changed_edges'], 33)
        self.assertEqual(m['edge_count'], 193)
        self.assertEqual(measure(list(range(4096)), 0)['changed_edges'], 1)

    def test_full_pcm16_edge_range_no_overflow(self):
        pcm = [0] * 4096
        pcm[110], pcm[111] = -32768, 32767
        m = measure(pcm, 32)
        self.assertEqual(m['shifted_source_edge_peak'], 65535)
        self.assertEqual(m['changed_edge_peak'], 0)

    def test_tampered_render_is_rejected(self):
        pcm = [0] * 4096
        out, wraps = render_loop(pcm, 32, 4193)
        out[4096 + 80] = 1
        with self.assertRaises(ValueError):
            matched_peak_metrics(pcm, 32, out, wraps[0])

    def test_missing_nonfinite_or_partial_evidence_fails_closed(self):
        rows = selection_fixture([0] * 4096, 32)
        variants = [None, {}, {**rows[1]['source_matched_peak'], 'changed_edge_peak': float('nan')},
                    {**rows[1]['source_matched_peak'], 'edge_count': 100},
                    {**rows[1]['source_matched_peak'], 'source_edges_verified': False},
                    {**rows[1]['source_matched_peak'], 'reference_near_peak': 1}]
        for value in variants:
            r = choose([rows[0], {**rows[1], 'source_matched_peak': value}], 4096, CTX,
                       peak_mode='source_matched')
            self.assertEqual(r['selected_fade_frames'], 0)
            self.assertIn('PEAK_ORIGIN_UNVERIFIED', r['candidates'][1]['rejection_reasons'])

    def test_non_peak_gates_remain_and_runtime_stays_disabled(self):
        pcm = [0] * 4096
        pcm[110] = 20000
        rows = selection_fixture(pcm, 32)
        for change, reason in ((dict(spectral_distance=.9), 'HIGH_LOCAL_SPECTRAL_DISTANCE'),
                               (dict(lag_shift=1), 'LOCAL_LAG_SHIFT_REQUIRES_REVIEW'),
                               (dict(near_rms=1200), 'NEAR_SEAM_RMS_WORSE')):
            r = choose([rows[0], {**rows[1], **change}], 4096, CTX, peak_mode='source_matched')
            self.assertIn(reason, r['candidates'][1]['rejection_reasons'])
            self.assertEqual(r['selected_fade_frames'], 0)
            self.assertFalse(r['runtime_approved'])
            self.assertEqual(r['runtime_fade_frames'], 0)

    def test_analysis_uses_v3_and_records_legacy_shadow(self):
        r = analyze([0] * 4096, CTX)
        self.assertEqual(r['policy_version'], POLICY_VERSION)
        self.assertEqual(r['legacy_window_v2']['policy_version'], LEGACY_POLICY_VERSION)
        self.assertTrue(all('source_matched_peak' in c for c in r['candidates']))
        self.assertEqual(r['decision'], 'DIRECT_NO_BENEFIT')

    def test_reordered_candidates_stable_and_unknown_mode_rejected(self):
        pcm = [0] * 4096
        pcm[110] = 20000
        rows = selection_fixture(pcm, 32)
        a = choose(rows, 4096, CTX, peak_mode='source_matched')
        b = choose(list(reversed(rows)), 4096, CTX, peak_mode='source_matched')
        self.assertEqual(a['selected_fade_frames'], b['selected_fade_frames'])
        with self.assertRaises(ValueError):
            choose(rows, 4096, CTX, peak_mode='typo')

    def test_evidence_structural_corruption_rejected(self):
        m = measure([0] * 4096, 32)
        for key, value in [('changed_edges', True), ('shifted_source_edges', 300),
                           ('candidate_near_peak', -1), ('schema', 'old'),
                           ('unchanged_edge_peak', 5), ('changed_edge_peak', 65536)]:
            bad = {**m, key: value}
            self.assertFalse(valid_peak_evidence(bad, 0, 0, 4096))

    def test_bad_geometry_and_incomplete_render_rejected(self):
        for pcm, fade, out, wrap in [([0]*3, 0, [0]*10, 3),
                                     ([0]*10, 5, [0]*20, 10),
                                     ([0]*10, 0, [0]*12, 10),
                                     ([0]*10, 0, [0]*20, 11)]:
            with self.assertRaises(ValueError):
                matched_peak_metrics(pcm, fade, out, wrap)


if __name__ == '__main__':
    unittest.main()
