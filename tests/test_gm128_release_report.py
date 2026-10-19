import unittest

from tools.gm128_release.report import parse_log, render_markdown, report_json


def make_log(*, fail_program=None, missing_melodic=None, summary_prepare_failures=0):
    lines = []
    max_aligned = -1
    max_bank = 0
    max_program = 0
    max_zones = -1
    max_zones_bank = 0
    max_zones_program = 0
    max_deps = -1
    max_deps_bank = 0
    max_deps_program = 0
    rows = 0
    melodic_count = 0
    percussion_count = 0

    for bank, program_class, channel in ((0, "melodic", 0), (128, "percussion", 9)):
        for program in range(128):
            if bank == 0 and program == missing_melodic:
                continue
            zones = 1 + (program % 7)
            deps = 1 + (program % 5)
            aligned = 10000 + bank * 10 + program * 1000
            status = "FAIL" if (bank, program) == fail_program else "PASS"
            coverage_misses = 1 if status == "FAIL" else 0
            lines.append(
                "M7_PROGRAM "
                f"bank={bank} program={program} class={program_class} status={status} "
                f"channel={channel} native_zones={zones} zones={zones} deps={deps} "
                f"stereo_pairs=0 pcm_bytes={aligned - 4} aligned_pcm_bytes={aligned} "
                "supported_modulators=0 unsupported_modulators=0 preview_zones=0 "
                f"coverage_misses={coverage_misses} "
                f"drum_checked={1 if bank == 128 and program == 0 else 0}"
            )
            rows += 1
            if bank == 0:
                melodic_count += 1
            else:
                percussion_count += 1
            if aligned > max_aligned:
                max_aligned = aligned
                max_bank = bank
                max_program = program
            if zones > max_zones:
                max_zones = zones
                max_zones_bank = bank
                max_zones_program = program
            if deps > max_deps:
                max_deps = deps
                max_deps_bank = bank
                max_deps_program = program

    lines.append(
        "M7_MATRIX_SUMMARY "
        f"rows={rows} melodic_programs={melodic_count} percussion_programs={percussion_count} "
        f"missing_melodic={128 - melodic_count} missing_percussion={128 - percussion_count} "
        f"prepare_failures={summary_prepare_failures} "
        f"coverage_failures={1 if fail_program is not None else 0} "
        "unsupported_modulators=0 preview_zones=0 "
        f"max_zones={max_zones} max_zones_bank={max_zones_bank} "
        f"max_zones_program={max_zones_program} "
        f"max_dependencies={max_deps} max_dependencies_bank={max_deps_bank} "
        f"max_dependencies_program={max_deps_program} "
        f"max_aligned_pcm_bytes={max_aligned} max_pcm_bank={max_bank} "
        f"max_pcm_program={max_program}"
    )
    return "\n".join(lines)


class Gm128ReleaseReportTests(unittest.TestCase):
    def test_complete_matrix_passes_and_renders(self):
        report = parse_log(make_log())
        self.assertTrue(report.passed, report.errors)
        self.assertEqual(len(report.programs), 256)
        self.assertIsNotNone(report.summary)
        self.assertEqual(report.summary.melodic_programs, 128)
        self.assertEqual(report.summary.percussion_programs, 128)
        payload = report_json(report)
        self.assertEqual(payload["schema"], "gm128-release-matrix-v1")
        self.assertTrue(payload["passed"])
        markdown = render_markdown(report)
        self.assertIn("Status: **PASS**", markdown)
        self.assertIn("melodic Bank 0 programs: `128/128`", markdown)
        self.assertIn("Largest PCM programs", markdown)

    def test_missing_melodic_program_fails(self):
        report = parse_log(make_log(missing_melodic=64))
        self.assertFalse(report.passed)
        self.assertTrue(any("missing melodic programs" in error for error in report.errors))

    def test_program_failure_is_preserved(self):
        report = parse_log(make_log(fail_program=(128, 0)))
        self.assertFalse(report.passed)
        self.assertTrue(any("program failed bank=128 program=0" in error for error in report.errors))
        self.assertTrue(any("coverage misses bank=128 program=0" in error for error in report.errors))

    def test_summary_failure_is_rejected(self):
        report = parse_log(make_log(summary_prepare_failures=1))
        self.assertFalse(report.passed)
        self.assertTrue(any("summary prepare_failures=1" in error for error in report.errors))


if __name__ == "__main__":
    unittest.main()
