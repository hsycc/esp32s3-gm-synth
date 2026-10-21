import tempfile
import unittest
from pathlib import Path

from tools.m7_release import modulator_closure


class M7ModulatorClosureTests(unittest.TestCase):
    def test_currently_supported_routes_are_filtered(self):
        route = modulator_closure.Route(
            asset="fixture",
            scope="instrument",
            source=0x0502,
            destination=48,
            amount=960,
            amount_source=0,
            transform=0,
            count=1,
        )
        self.assertTrue(modulator_closure.currently_supported(route))

    def test_fluid_dual_velocity_filter_q_matches_runtime_prepared_route(self):
        route = modulator_closure.Route(
            asset="fluidr3_gm",
            scope="instrument",
            source=0x0402,
            destination=9,
            amount=-470,
            amount_source=0x0D02,
            transform=0,
            count=4,
        )
        self.assertTrue(modulator_closure.currently_supported(route))

    def test_note_on_pan_remains_explicitly_pending(self):
        route = modulator_closure.Route(
            asset="generaluser_gs",
            scope="instrument",
            source=0x0203,
            destination=17,
            amount=400,
            amount_source=0,
            transform=0,
            count=1,
        )
        self.assertFalse(modulator_closure.currently_supported(route))
        self.assertEqual(
            modulator_closure.classify(route),
            "prepared_note_on_candidate",
        )

    def test_sample_address_and_arbitrary_cc_stay_separate(self):
        sample_address = modulator_closure.Route(
            asset="generaluser_gs",
            scope="instrument",
            source=0x0002,
            destination=0,
            amount=-2000,
            amount_source=0,
            transform=0,
            count=6,
        )
        cc21_filter = modulator_closure.Route(
            asset="generaluser_gs",
            scope="preset",
            source=0x0195,
            destination=8,
            amount=-3800,
            amount_source=0,
            transform=0,
            count=1,
        )
        self.assertEqual(
            modulator_closure.classify(sample_address),
            "sample_address_note_on",
        )
        self.assertEqual(
            modulator_closure.classify(cc21_filter),
            "controller_state_pending",
        )

    def test_inventory_parser_preserves_signed_amount_and_counts(self):
        text = (
            "M7_MOD_ROUTE asset=fluidr3_gm scope=instrument "
            "source=0x0402 destination=9 amount=-470 "
            "amount_source=0x0d02 transform=0 count=4\n"
        )
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "inventory.log"
            path.write_text(text, encoding="utf-8")
            routes = modulator_closure.parse(path)
        self.assertEqual(len(routes), 1)
        self.assertEqual(routes[0].amount, -470)
        self.assertEqual(routes[0].count, 4)
        self.assertEqual(routes[0].amount_source, 0x0D02)


if __name__ == "__main__":
    unittest.main()
