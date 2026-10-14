import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from tools.sf2_analyzer.modulator import (
    build_mod_plan,
    decode_source,
    default_modulators,
    encode_source,
)
from tools.sf2_analyzer.modulator_eval import (
    evaluate_mod_plan,
    evaluate_modulator,
    map_source_value,
)


class ModulatorEvaluationTests(unittest.TestCase):
    def test_linear_unipolar_positive(self):
        source = decode_source(encode_source(1, cc=True))
        self.assertEqual(map_source_value(source, 0), 0.0)
        self.assertAlmostEqual(map_source_value(source, 64), 0.5)
        self.assertAlmostEqual(map_source_value(source, 127), 127 / 128)

    def test_linear_unipolar_negative(self):
        source = decode_source(encode_source(7, cc=True, negative=True))
        self.assertAlmostEqual(map_source_value(source, 0), 127 / 128)
        self.assertAlmostEqual(map_source_value(source, 127), 0.0)

    def test_linear_bipolar_center(self):
        source = decode_source(encode_source(10, cc=True, bipolar=True))
        self.assertAlmostEqual(map_source_value(source, 64), 0.0)
        self.assertLess(map_source_value(source, 0), 0.0)
        self.assertGreater(map_source_value(source, 127), 0.0)

    def test_switch_threshold(self):
        source = decode_source(encode_source(2, curve=3))
        self.assertEqual(map_source_value(source, 63), 0.0)
        self.assertEqual(map_source_value(source, 64), 1.0)

    def test_concave_and_convex_are_bounded(self):
        concave = decode_source(encode_source(2, curve=1))
        convex = decode_source(encode_source(2, curve=2))
        for value in (0, 1, 32, 64, 96, 127):
            self.assertGreaterEqual(map_source_value(concave, value), 0.0)
            self.assertLessEqual(map_source_value(concave, value), 127 / 128)
            self.assertGreaterEqual(map_source_value(convex, value), 0.0)
            self.assertLessEqual(map_source_value(convex, value), 127 / 128)

    def test_default_velocity_to_attenuation(self):
        mod = next(
            m
            for m in default_modulators()
            if m.source.name == "note_on_velocity" and m.destination == 48
        )
        self.assertAlmostEqual(evaluate_modulator(mod, {"velocity": 127}), 0.0)
        self.assertAlmostEqual(evaluate_modulator(mod, {"velocity": 0}), 952.5)

    def test_pitch_bend_center_is_zero(self):
        mod = next(
            m
            for m in default_modulators()
            if m.source.name == "pitch_wheel" and m.destination == 52
        )
        self.assertAlmostEqual(
            evaluate_modulator(
                mod,
                {"pitch_wheel": 8192, "pitch_wheel_sensitivity": 2},
            ),
            0.0,
        )

    def test_plan_can_recalculate_only_one_cc_group(self):
        plan = build_mod_plan(default_modulators())
        totals = evaluate_mod_plan(
            plan,
            {"velocity": 96, "key": 60, "cc": {10: 127}},
            trigger="cc:10",
        )
        self.assertEqual(set(totals), {17})
        self.assertGreater(totals[17], 0.0)


if __name__ == "__main__":
    unittest.main()
