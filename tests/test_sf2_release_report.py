import unittest

from tools.sf2_release.report import parse_asset_log, parse_portfolio, report_json


ACCEPTED = """
M7_SF2_ASSET asset=fm profile=gm128 required_accept=1 source_kind=full path=assets/fm.sf2 file_bytes=100 presets=1 samples=2
M7_SF2_PROGRAM asset=fm bank=0 program=0 class=melodic status=PASS reason=none channel=0 native_zones=1 zones=1 deps=1 stereo_pairs=0 pcm_bytes=20 aligned_pcm_bytes=32 representative_overlap=1 supported_modulators=0 unsupported_modulators=0 preview_zones=0 coverage_misses=0 coverage_required=1
M7_SF2_SUMMARY asset=fm profile=gm128 classification=ACCEPTED required_accept=1 source_kind=full file_bytes=100 presets=1 rows=1 banks=2 bank0_programs=128 bank128_programs=128 missing_bank0=0 missing_bank128=0 metadata_failures=0 duplicate_identities=0 program_range_failures=0 prepare_failures=0 dependency_failures=0 rejected_rows=0 unsupported_modulators=0 preview_zones=0 max_zones=1 max_zones_bank=0 max_zones_program=0 max_dependencies=1 max_dependencies_bank=0 max_dependencies_program=0 max_aligned_pcm_bytes=32 max_pcm_bank=0 max_pcm_program=0 max_representative_overlap=1 max_overlap_bank=0 max_overlap_program=0
"""

OPTIONAL_REJECTED = """
M7_SF2_ASSET asset=user profile=native required_accept=0 source_kind=full path=assets/user.sf2 file_bytes=200 presets=1 samples=3
M7_SF2_PROGRAM asset=user bank=0 program=0 class=melodic status=REJECTED reason=unsupported_modulator channel=0 native_zones=2 zones=2 deps=1 stereo_pairs=0 pcm_bytes=40 aligned_pcm_bytes=64 representative_overlap=2 supported_modulators=1 unsupported_modulators=1 preview_zones=0 coverage_misses=0 coverage_required=0
M7_SF2_SUMMARY asset=user profile=native classification=REJECTED required_accept=0 source_kind=full file_bytes=200 presets=1 rows=1 banks=1 bank0_programs=1 bank128_programs=0 missing_bank0=127 missing_bank128=128 metadata_failures=0 duplicate_identities=0 program_range_failures=0 prepare_failures=0 dependency_failures=0 rejected_rows=1 unsupported_modulators=1 preview_zones=0 max_zones=2 max_zones_bank=0 max_zones_program=0 max_dependencies=1 max_dependencies_bank=0 max_dependencies_program=0 max_aligned_pcm_bytes=64 max_pcm_bank=0 max_pcm_program=0 max_representative_overlap=2 max_overlap_bank=0 max_overlap_program=0
"""

ASSET_LIMIT_REJECTION = """
M7_SF2_ASSET_REJECTED asset=generaluser_gs profile=native required_accept=0 source_kind=full path=assets/GeneralUser-GS.sf2 reason=runtime_sample_table_limit file_bytes=32981394 sample_source_error=6 runtime_limit=512
"""


class Sf2ReleaseReportTests(unittest.TestCase):
    def test_required_accepted_asset_passes(self) -> None:
        report = parse_asset_log(ACCEPTED)
        self.assertTrue(report.scanned)
        self.assertTrue(report.accepted)
        self.assertTrue(report.passed)
        self.assertEqual(report.summary.max_aligned_pcm_bytes, 32)

    def test_optional_explicit_rejection_is_valid_classification(self) -> None:
        report = parse_asset_log(OPTIONAL_REJECTED)
        self.assertTrue(report.scanned)
        self.assertFalse(report.accepted)
        self.assertTrue(report.passed)
        self.assertEqual(report.programs[0].reason, "unsupported_modulator")

    def test_asset_level_runtime_limit_rejection_is_valid(self) -> None:
        report = parse_asset_log(ASSET_LIMIT_REJECTION)
        self.assertTrue(report.scanned)
        self.assertFalse(report.accepted)
        self.assertTrue(report.passed)
        self.assertEqual(report.rejection.reason, "runtime_sample_table_limit")
        self.assertEqual(report.rejection.runtime_limit, 512)

    def test_portfolio_allows_optional_rejections(self) -> None:
        portfolio = parse_portfolio(
            [ACCEPTED, OPTIONAL_REJECTED, ASSET_LIMIT_REJECTION], min_assets=3
        )
        self.assertTrue(portfolio.passed)
        payload = report_json(portfolio)
        self.assertEqual(payload["schema"], "sf2-release-portfolio-v1")
        self.assertEqual(payload["accepted_assets"], 1)
        self.assertEqual(payload["rejected_assets"], 2)

    def test_required_rejection_fails(self) -> None:
        required_rejected = ASSET_LIMIT_REJECTION.replace(
            "required_accept=0", "required_accept=1"
        )
        report = parse_asset_log(required_rejected)
        self.assertFalse(report.passed)
        self.assertTrue(any("required asset rejected" in error for error in report.errors))

    def test_minimum_asset_count_is_enforced(self) -> None:
        portfolio = parse_portfolio([ACCEPTED], min_assets=3)
        self.assertFalse(portfolio.passed)
        self.assertTrue(any("expected at least 3 assets" in error for error in portfolio.errors))

    def test_fatal_line_is_report_error(self) -> None:
        report = parse_asset_log(ACCEPTED + "M7_SF2_FATAL asset=fm reason=panic\n")
        self.assertFalse(report.scanned)
        self.assertFalse(report.passed)

    def test_rejection_must_not_mix_with_program_records(self) -> None:
        report = parse_asset_log(ASSET_LIMIT_REJECTION + ACCEPTED)
        self.assertFalse(report.passed)
        self.assertTrue(any("must not mix" in error for error in report.errors))


if __name__ == "__main__":
    unittest.main()
