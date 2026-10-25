"""Conservative, host-only loop crossfade selection tests."""
import unittest

from tools.v1_loop_crossfade_planner import evaluate


class PlannerTests(unittest.TestCase):
    def test_short_loop_rejected(self):
        with self.assertRaises(ValueError):
            evaluate([0, 1, 2])

    def test_requires_direct_reference(self):
        with self.assertRaises(ValueError):
            evaluate([0] * 256, (16, 32))

    def test_rejects_duplicate_or_out_of_range_fades(self):
        with self.assertRaises(ValueError):
            evaluate([0] * 256, (0, 16, 16))
        with self.assertRaises(ValueError):
            evaluate([0] * 256, (0, 128))

    def test_smooth_loop_keeps_direct(self):
        result = evaluate([400] * 4096)
        self.assertEqual(result["selected_fade_frames"], 0)
        self.assertEqual(result["decision"], "DIRECT")

    def test_small_seam_keeps_direct_even_if_fade_reduces_it(self):
        result = evaluate([0] * 4032 + [1000] * 64)
        self.assertEqual(result["selected_fade_frames"], 0)

    def test_large_seam_can_choose_short_bounded_fade(self):
        result = evaluate([0] * 4032 + [20000] * 64)
        self.assertIn(result["selected_fade_frames"], (16, 32, 64))
        self.assertEqual(result["decision"], "CANDIDATE_ONLY")
        self.assertLess(
            next(c["seam_step"] for c in result["candidates"]
                 if c["fade_frames"] == result["selected_fade_frames"]),
            result["candidates"][0]["seam_step"])
        self.assertTrue(result["limitations"])

    def test_waveform_diagnostics_are_finite_and_reported(self):
        result = evaluate([0] * 4032 + [20000] * 64)
        self.assertEqual(len(result["candidates"]), 4)
        for candidate in result["candidates"]:
            self.assertGreaterEqual(candidate["overlap_change_rms"], 0)
            self.assertGreaterEqual(candidate["overlap_derivative_rms"], 0)
            self.assertIn("derivative_ratio", candidate)
        self.assertEqual(result["candidates"][0]["overlap_change_rms"], 0)

    def test_local_spectral_diagnostics_are_bounded(self):
        import math
        # Mixed harmonics exercise spectral shape and finite lag estimates.
        pcm = [round(12000*math.sin(2*math.pi*i/31) +
                     2000*math.sin(2*math.pi*i/9)) for i in range(4096)]
        result = evaluate(pcm)
        self.assertEqual(len(result["candidates"]), 4)
        self.assertIsNone(result["candidates"][0]["spectral_distance"])
        for c in result["candidates"][1:]:
            self.assertIn(c["spectral_status"],
                          ("LOCAL_DIAGNOSTIC_ONLY", "INSUFFICIENT_SIGNAL"))
            if c["spectral_distance"] is not None:
                self.assertGreaterEqual(c["spectral_distance"], 0)
                self.assertLessEqual(c["spectral_distance"], 2.00001)

    def test_silence_reports_insufficient_signal(self):
        result = evaluate([0]*4096)
        self.assertEqual(result["selected_fade_frames"], 0)
        self.assertTrue(all(
            c["spectral_status"] == "INSUFFICIENT_SIGNAL"
            for c in result["candidates"][1:]))

    def test_period_budget_rejects_long_fades(self):
        # All fades exceed the conservative period-change budget.
        result = evaluate([0] * 224 + [20000] * 32, (0, 16, 32, 64))
        self.assertEqual(result["selected_fade_frames"], 0)


if __name__ == "__main__":
    unittest.main()
