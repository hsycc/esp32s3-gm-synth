import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from tools.sf2_analyzer.modulator import (
    Modulator,
    build_mod_plan,
    decode_source,
    default_modulators,
    encode_source,
    merge_modulators,
)


class ModulatorTests(unittest.TestCase):
    def test_source_bitfield_decode(self):
        raw = encode_source(10, cc=True, negative=True, bipolar=True, curve=2)
        source = decode_source(raw)
        self.assertEqual(source.index, 10)
        self.assertTrue(source.is_cc)
        self.assertTrue(source.negative)
        self.assertTrue(source.bipolar)
        self.assertEqual(source.curve_name, "convex")
        self.assertEqual(source.name, "cc_10")
        self.assertEqual(source.trigger_group, "cc:10")

    def test_default_modulator_set_contains_expected_routes(self):
        defaults = default_modulators()
        routes = {(m.source.name, m.destination_name, m.amount) for m in defaults}
        self.assertIn(("note_on_velocity", "initialAttenuation", 960), routes)
        self.assertIn(("cc_1", "vibLfoToPitch", 50), routes)
        self.assertIn(("cc_10", "pan", 500), routes)
        self.assertIn(("cc_91", "reverbEffectsSend", 200), routes)
        self.assertIn(("cc_93", "chorusEffectsSend", 200), routes)
        self.assertIn(("pitch_wheel", "fineTune", 12700), routes)

    def test_zero_amount_instrument_modulator_disables_identical_default(self):
        default = next(m for m in default_modulators() if m.source.name == "cc_1" and m.destination == 6)
        disable = Modulator(
            default.source_raw,
            default.destination,
            0,
            default.amount_source_raw,
            default.transform,
            "instrument_local",
        )
        merged = merge_modulators(instrument_local=[disable])
        effective = next(m for m in merged if m.identity == default.identity)
        self.assertEqual(effective.amount, 0)
        self.assertTrue(effective.disabled)
        self.assertEqual(effective.origin, "instrument_local")

    def test_zero_amount_preset_modulator_does_not_disable_instrument(self):
        src = encode_source(1, cc=True)
        instrument = Modulator(src, 6, 80, 0, 0, "instrument_local")
        preset_zero = Modulator(src, 6, 0, 0, 0, "preset_local")
        merged = merge_modulators(
            instrument_local=[instrument],
            preset_local=[preset_zero],
            defaults=[],
        )
        self.assertEqual(len(merged), 1)
        self.assertEqual(merged[0].amount, 80)

    def test_preset_adds_to_identical_instrument_modulator(self):
        src = encode_source(1, cc=True)
        instrument = Modulator(src, 6, 50, 0, 0, "instrument_local")
        preset = Modulator(src, 6, 25, 0, 0, "preset_local")
        merged = merge_modulators(
            instrument_local=[instrument],
            preset_local=[preset],
            defaults=[],
        )
        self.assertEqual(len(merged), 1)
        self.assertEqual(merged[0].amount, 75)
        self.assertEqual(merged[0].origin, "instrument_local+preset_local")

    def test_local_modulator_supersedes_identical_global_modulator(self):
        src = encode_source(13)
        global_mod = Modulator(src, 6, 10, 0, 0, "instrument_global")
        local_mod = Modulator(src, 6, 40, 0, 0, "instrument_local")
        merged = merge_modulators(
            instrument_global=[global_mod],
            instrument_local=[local_mod],
            defaults=[],
        )
        self.assertEqual(len(merged), 1)
        self.assertEqual(merged[0].amount, 40)
        self.assertEqual(merged[0].origin, "instrument_local")

    def test_mod_plan_is_event_grouped_not_per_sample_interpreted(self):
        plan = build_mod_plan(default_modulators())
        self.assertIn("note_on", plan["groups"])
        self.assertIn("cc:1", plan["groups"])
        self.assertIn("cc:7", plan["groups"])
        self.assertIn("cc:10", plan["groups"])
        self.assertIn("pitch", plan["groups"])
        self.assertIn("channel_pressure", plan["groups"])
        self.assertNotIn("audio_sample", plan["groups"])


if __name__ == "__main__":
    unittest.main()
