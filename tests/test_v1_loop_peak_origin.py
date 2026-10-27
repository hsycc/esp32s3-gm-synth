import unittest
from tools.v1_loop_peak_origin import traced_loop, inspect_peak


class PeakOriginTests(unittest.TestCase):
    def test_direct_sequence_and_wrap(self):
        pcm = list(range(200))
        out, origins, wraps = traced_loop(pcm, 0, 301)
        self.assertEqual(out, pcm+pcm[:101])
        self.assertEqual(wraps, [200])
        self.assertEqual(origins[201], ('source', 1))

    def test_overlap_resumes_at_fade(self):
        pcm = list(range(200))
        out, origins, wraps = traced_loop(pcm, 16, 220)
        self.assertEqual(origins[199], ('blend', 199, 15))
        self.assertEqual(origins[200], ('source', 16))
        self.assertEqual(out[200], pcm[16])
        self.assertEqual(wraps, [200])

    def test_unchanged_peak_shifted_into_window_is_identified(self):
        pcm = [0]*4096
        pcm[110] = 20000
        r = inspect_peak(pcm, 32)
        self.assertEqual(r['direct_near_peak'], 0)
        self.assertEqual(r['candidate_near_peak'], 20000)
        self.assertTrue(r['all_maxima_unchanged_source'])
        self.assertTrue(r['all_maxima_absent_from_direct_window'])
        self.assertEqual(r['max_edges'][0]['current_origin'], ['source', 110])

    def test_mixed_edges_are_not_claimed_unchanged(self):
        pcm = [0]*4032 + [20000]*64
        r = inspect_peak(pcm, 32)
        # The original rise at -64 is inside both windows.
        self.assertTrue(r['all_maxima_unchanged_source'])
        pcm = [0]*4095+[20000]
        r = inspect_peak(pcm, 32)
        self.assertFalse(r['all_maxima_unchanged_source'])

    def test_short_loop_multiple_wraps(self):
        out, _, wraps = traced_loop(list(range(10)), 2, 31)
        self.assertEqual(len(out), 31)
        self.assertEqual(wraps, [10, 18, 26])

    def test_invalid_geometry_rejected(self):
        for pcm, fade, total in (([0]*3, 0, 10), ([0]*10, 5, 20), ([0]*10, 0, 10)):
            with self.assertRaises(ValueError):
                traced_loop(pcm, fade, total)


if __name__ == '__main__':
    unittest.main()
