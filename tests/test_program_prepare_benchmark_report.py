from __future__ import annotations

import unittest

from tools.program_prepare_benchmark.report import parse_log, render_markdown


PASS_LOG = """
I (4200) prepare_bench: start path=/sf2/test.sf2 samples=67 arena=4194304 A[count=5 aligned=540000] B[count=7 aligned=710000] read_chunk=32768
I (4300) prepare_bench: case=cold_initial ok=1 elapsed_us=91000 dependencies=5 dependency_pcm_bytes=539990 dependency_aligned_bytes=540000 loaded=5 loaded_bytes=539990 reused_resident=0 evictions=0 evicted_bytes=0 capacity_retries=0
I (4301) prepare_bench: case=warm_same_program ok=1 elapsed_us=120 dependencies=5 dependency_pcm_bytes=539990 dependency_aligned_bytes=540000 loaded=0 loaded_bytes=0 reused_resident=5 evictions=0 evicted_bytes=0 capacity_retries=0
I (4302) prepare_bench: case=warm_commit_retire commit_ok=1 ack_ok=1 commit_us=2 retire_us=35
I (4900) prepare_bench: case=prime_cache ok=1 elapsed_us=590000 loaded_samples=48 loaded_bytes=3550000 used=4090000 largest_free_span=104304 free_spans=3
I (5050) prepare_bench: case=eviction_program_change ok=1 elapsed_us=145000 dependencies=7 dependency_pcm_bytes=709980 dependency_aligned_bytes=710000 loaded=7 loaded_bytes=709980 reused_resident=0 evictions=8 evicted_bytes=760000 capacity_retries=8
I (5051) prepare_bench: case=eviction_commit_retire commit_ok=1 ack_ok=1 commit_us=3 retire_us=41 release_tail_survived=1 release_ok=1
I (5052) prepare_bench: PASS used=4040000 high_water=4194200 largest_free_span=150000 resident_samples=52 total_prepare_successes=3 total_loaded_bytes=1249970 total_evictions=8 total_evicted_bytes=760000
I (5053) gm_synth: M3 core linked successfully; storage/I2S/Voice DSP are intentionally not started in this bring-up target
"""


class ProgramPrepareReportTests(unittest.TestCase):
    def test_pass_log(self) -> None:
        report = parse_log(PASS_LOG)
        self.assertTrue(report.passed, report.errors)
        self.assertIsNotNone(report.cold)
        self.assertIsNotNone(report.warm)
        self.assertIsNotNone(report.eviction)
        self.assertEqual(report.read_chunk_bytes, 32768)
        self.assertEqual(report.warm.loaded, 0)
        self.assertEqual(report.warm.reused_resident, 5)
        self.assertEqual(report.eviction.evictions, 8)
        self.assertTrue(report.eviction_commit.release_tail_survived)
        self.assertEqual(report.final.high_water_bytes, 4194200)

        markdown = render_markdown(report, title="N16R8 Program Prepare")
        self.assertIn("Status: **PASS**", markdown)
        self.assertIn("eviction_program_change", markdown)
        self.assertIn("Release tail survived", markdown)

    def test_missing_eviction_fails(self) -> None:
        broken = PASS_LOG.replace("evictions=8", "evictions=0").replace(
            "total_evictions=8", "total_evictions=0"
        )
        report = parse_log(broken)
        self.assertFalse(report.passed)
        self.assertIn(
            "eviction_program_change did not exercise cache eviction",
            report.errors,
        )

    def test_release_tail_failure_fails(self) -> None:
        broken = PASS_LOG.replace("release_tail_survived=1", "release_tail_survived=0")
        report = parse_log(broken)
        self.assertFalse(report.passed)
        self.assertIn("release-tail Voice Ref did not survive retire", report.errors)

    def test_panic_signature_fails(self) -> None:
        report = parse_log(PASS_LOG + "\nGuru Meditation Error: Core 0 panic'ed\n")
        self.assertFalse(report.passed)
        self.assertTrue(any("Guru Meditation" in error for error in report.errors))
        self.assertTrue(any("panic'ed" in error for error in report.errors))

    def test_sample_count_mismatch_fails(self) -> None:
        report = parse_log(PASS_LOG, expected_samples=66)
        self.assertFalse(report.passed)
        self.assertIn("sample-count mismatch: expected 66, got 67", report.errors)


if __name__ == "__main__":
    unittest.main()
