import unittest

from tools.render_benchmark.report import parse_cases, render_markdown, validate_cases


class RenderBenchmarkReportTests(unittest.TestCase):
    def test_parse_validate_and_render(self):
        text = "\n".join(
            [
                "I (1) gm_synth: render_bench: case voices=4 blocks=128 min_us=10 avg_us=12 p50_us=12 p95_us=14 p99_us=15 max_us=16 deadline_misses=0 clipped_samples=0 checksum=00A1",
                "I (2) gm_synth: render_bench: case voices=8 blocks=128 min_us=20 avg_us=24 p50_us=23 p95_us=28 p99_us=30 max_us=31 deadline_misses=1 clipped_samples=0 checksum=00B2",
                "I (3) gm_synth: render_bench: PASS cases=2 final_refs=0 clock_faults=0 duration_clamps=0 qemu_timing_regression_only=1",
            ]
        )
        cases = validate_cases(text, parse_cases(text), [4, 8])
        self.assertEqual([case.voices for case in cases], [4, 8])
        self.assertEqual(cases[1].p99_us, 30)
        self.assertEqual(cases[0].checksum, "00a1")

        markdown = render_markdown(cases, "Test Render Telemetry")
        self.assertIn("# Test Render Telemetry", markdown)
        self.assertIn("| 8 | 128 | 20 | 24 | 23 | 28 | 30 | 31 | 1 | 0 | `00b2` |", markdown)
        self.assertIn("regression telemetry only", markdown)

    def test_requires_pass_marker(self):
        text = "render_bench: case voices=4 blocks=128 min_us=1 avg_us=1 p50_us=1 p95_us=1 p99_us=1 max_us=1 deadline_misses=0 clipped_samples=0 checksum=aa"
        with self.assertRaisesRegex(ValueError, "PASS marker"):
            validate_cases(text, parse_cases(text), [4])

    def test_rejects_missing_voice_case(self):
        text = "\n".join(
            [
                "render_bench: case voices=4 blocks=128 min_us=1 avg_us=1 p50_us=1 p95_us=1 p99_us=1 max_us=1 deadline_misses=0 clipped_samples=0 checksum=aa",
                "render_bench: PASS",
            ]
        )
        with self.assertRaisesRegex(ValueError, "voices mismatch"):
            validate_cases(text, parse_cases(text), [4, 8])

    def test_rejects_invalid_percentile_order(self):
        text = "\n".join(
            [
                "render_bench: case voices=4 blocks=128 min_us=1 avg_us=3 p50_us=4 p95_us=3 p99_us=5 max_us=6 deadline_misses=0 clipped_samples=0 checksum=aa",
                "render_bench: PASS",
            ]
        )
        with self.assertRaisesRegex(ValueError, "percentiles"):
            validate_cases(text, parse_cases(text), [4])


if __name__ == "__main__":
    unittest.main()
