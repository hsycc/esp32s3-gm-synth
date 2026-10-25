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

    def test_period_budget_rejects_long_fades(self):
        # All fades exceed the conservative period-change budget.
        result = evaluate([0] * 224 + [20000] * 32, (0, 16, 32, 64))
        self.assertEqual(result["selected_fade_frames"], 0)


if __name__ == "__main__":
    unittest.main()
