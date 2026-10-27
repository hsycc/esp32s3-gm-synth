"""Bounded SF2 loop survey selection and output tests."""
import tempfile
import unittest
from pathlib import Path

from tools.v1_loop_crossfade_survey import selected_headers, classify, report, survey


class SurveyTests(unittest.TestCase):
    def test_length_stratified_selection_is_deterministic(self):
        headers=[dict(sample_id=i, loop_frames=200+i*101) for i in range(40)]
        first=selected_headers(headers, 7)
        self.assertEqual(first, selected_headers(list(reversed(headers)), 7))
        self.assertEqual(len(first), 7)
        self.assertEqual(first[0]["sample_id"], 0)
        self.assertEqual(first[-1]["sample_id"], 39)

    def test_select_all_and_bad_limit(self):
        headers=[dict(sample_id=i, loop_frames=i+200) for i in range(3)]
        self.assertEqual(len(selected_headers(headers, 10)), 3)
        with self.assertRaises(ValueError):
            selected_headers(headers, 0)

    def test_review_flags_do_not_change_decision(self):
        candidate={"fade_frames": 32, "spectral_distance": .35,
                   "lag_shift": 2, "period_change_fraction": .012}
        result={"selected_fade_frames": 32, "candidates": [candidate]}
        self.assertEqual(set(classify(result)), {
            "HIGH_LOCAL_SPECTRAL_DISTANCE", "LOCAL_LAG_SHIFT",
            "PERIOD_CHANGE_GE_1PCT"})

    def test_direct_does_not_create_false_review_flags(self):
        result={"selected_fade_frames": 0,
                "candidates": [{"fade_frames": 0, "spectral_distance": None,
                                "lag_shift": None, "period_change_fraction": 0}]}
        self.assertEqual(classify(result), [])

    def test_malformed_file_fails_explicitly(self):
        with tempfile.TemporaryDirectory() as td:
            path=Path(td)/"bad.sf2"
            path.write_bytes(b"not an sf2")
            with self.assertRaises(ValueError):
                survey(path)

    def test_markdown_notes_raw_shdr_scope(self):
        value={"scope": "raw shdr", "evaluated": 0, "eligible_headers": 0,
               "selected_headers": 0, "fade_counts": {}, "skip_counts": {},
               "rows": []}
        text=report(value)
        self.assertIn("not a firmware plan", text)
        self.assertIn("not directly comparable", text)


if __name__ == "__main__":
    unittest.main()
