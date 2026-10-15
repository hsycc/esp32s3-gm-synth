from __future__ import annotations

import unittest
from pathlib import Path

from tools.residency_fixture.generate_cpp_fixture import load_bank_fixture, pressure_summary


ROOT = Path(__file__).resolve().parents[2]
MANIFEST_DIR = ROOT / "assets" / "sf2" / "manifests"
MANIFESTS = [
    MANIFEST_DIR / "FM_GM_Open_subset_6MiB.analysis.json",
    MANIFEST_DIR / "GeneralUser_GS_subset_6MiB.analysis.json",
    MANIFEST_DIR / "FluidR3_GM_subset_6MiB.analysis.json",
]


class RealBankResidencyManifestTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.fixtures = [load_bank_fixture(path) for path in MANIFESTS]
        cls.by_name = {fixture.name: fixture for fixture in cls.fixtures}

    def test_three_qualification_banks_validate(self) -> None:
        self.assertEqual(len(self.fixtures), 3)
        for fixture in self.fixtures:
            self.assertGreater(fixture.aligned_pcm_bytes, 0)
            self.assertGreaterEqual(
                fixture.all_presets_aligned_pcm_bytes,
                fixture.aligned_pcm_bytes,
            )
            self.assertGreater(len(fixture.programs), 0)
            self.assertLessEqual(len(fixture.programs), 16)
            self.assertGreater(len(fixture.unique_sample_ids), 0)
            self.assertLessEqual(max(fixture.unique_sample_ids), 511)

    def test_fm_gm_compact_bank_has_small_16_channel_union(self) -> None:
        fixture = self.by_name["FM_GM_Open_subset_6MiB"]
        self.assertEqual(fixture.aligned_pcm_bytes, 2_221_452)
        self.assertEqual(len(fixture.programs), 16)
        self.assertEqual(len(fixture.unique_sample_ids), 16)
        self.assertLess(fixture.aligned_pcm_bytes, 4 * 1024 * 1024)

    def test_generaluser_16_channel_union_nearly_spans_bank(self) -> None:
        fixture = self.by_name["GeneralUser_GS_subset_6MiB"]
        self.assertEqual(fixture.aligned_pcm_bytes, 5_675_820)
        self.assertEqual(fixture.all_presets_aligned_pcm_bytes, 5_862_084)
        self.assertEqual(len(fixture.programs), 16)
        self.assertEqual(len(fixture.unique_sample_ids), 121)
        self.assertGreater(
            fixture.aligned_pcm_bytes / fixture.all_presets_aligned_pcm_bytes,
            0.95,
        )

    def test_budget_report_exposes_r8_pressure_without_assuming_reserve(self) -> None:
        report = pressure_summary(self.fixtures, [2, 4, 5, 6])
        banks = {item["name"]: item for item in report["banks"]}

        fm = banks["FM_GM_Open_subset_6MiB"]
        general_user = banks["GeneralUser_GS_subset_6MiB"]

        fm_by_budget = {item["budget_bytes"]: item for item in fm["budgets"]}
        gu_by_budget = {item["budget_bytes"]: item for item in general_user["budgets"]}

        mib = 1024 * 1024
        self.assertFalse(fm_by_budget[2 * mib]["fits_greedy_16"])
        self.assertTrue(fm_by_budget[4 * mib]["fits_greedy_16"])

        self.assertFalse(gu_by_budget[5 * mib]["fits_greedy_16"])
        self.assertTrue(gu_by_budget[6 * mib]["fits_greedy_16"])
        self.assertFalse(gu_by_budget[5 * mib]["fits_all_presets"])
        self.assertTrue(gu_by_budget[6 * mib]["fits_all_presets"])


if __name__ == "__main__":
    unittest.main()
