import unittest

from tools.m6_qualification.report import parse_log, render_markdown


VOICE_COUNTS = (1, 4, 8, 12, 16, 18, 20, 24)


def make_log(*, physical=True, over_budget_from=None, stale_faults=0, arena=4_718_592, omit_voice=None):
    lines = [
        "I gm_synth: Detected physical PSRAM: 8388608 bytes (8.00 MiB)",
        "I gm_synth: PSRAM before arena: total=8388608 free=8380000 largest=8257536 minimum_free=8370000 bytes",
        "I gm_synth: INTERNAL_HEAP before arena: total=330000 free=250000 largest=120000 minimum_free=245000 bytes",
        f"I gm_synth: Requesting PCM residency arena: {arena} bytes (4.50 MiB)",
        f"I gm_synth: PSRAM after arena: total=8388608 free=3650000 largest=3500000 minimum_free=3600000 bytes",
        "I gm_synth: INTERNAL_HEAP after arena: total=330000 free=248000 largest=118000 minimum_free=243000 bytes",
        "I gm_synth: Benchmark FATFS mounted: base=/sf2 partition=qemu_sf2",
        "I gm_synth: Storage benchmark VFS open: path=/sf2/test.sf2 file_bytes=5898214",
        "I gm_synth: Storage benchmark SF2 parsed: playable_samples=67",
        "I gm_synth: Storage benchmark source: path=/sf2/test.sf2 smpl_offset=1000 smpl_bytes=5000000 benchmark_bytes=4194304",
    ]
    for index, chunk in enumerate((4096, 8192, 16384, 32768, 65536), start=1):
        lines.append(
            f"I gm_synth: chunk={chunk} bytes=4194304 chunks={1024 // index} elapsed={700000-index*10000} us "
            f"throughput={5.8 + index/10:.3f} MiB/s latency_us[min/avg/p50/p95/p99/max]={100*index}/{110*index}/{108*index}/{120*index}/{130*index}/{150*index}"
        )
    lines.extend(
        [
            f"I gm_synth: Residency ready: capacity={arena} used=0 largest_free_span={arena} free_spans=1",
            f"I gm_synth: prepare_bench: start path=/sf2/test.sf2 samples=67 arena={arena} A[count=2 aligned=1000] B[count=3 aligned=2000] read_chunk=32768",
            "I gm_synth: prepare_bench: case=cold_initial ok=1 elapsed_us=1200 dependencies=2 dependency_pcm_bytes=900 dependency_aligned_bytes=1000 loaded=2 loaded_bytes=900 reused_resident=0 evictions=0 evicted_bytes=0 capacity_retries=0",
            "I gm_synth: prepare_bench: case=warm_same_program ok=1 elapsed_us=40 dependencies=2 dependency_pcm_bytes=900 dependency_aligned_bytes=1000 loaded=0 loaded_bytes=0 reused_resident=2 evictions=0 evicted_bytes=0 capacity_retries=0",
            "I gm_synth: prepare_bench: case=warm_commit_retire commit_ok=1 ack_ok=1 commit_us=3 retire_us=4",
            "I gm_synth: prepare_bench: case=prime_cache ok=1 elapsed_us=2000 loaded_samples=4 loaded_bytes=3000 used=3900 largest_free_span=4700000 free_spans=2",
            "I gm_synth: prepare_bench: case=eviction_program_change ok=1 elapsed_us=1500 dependencies=3 dependency_pcm_bytes=1800 dependency_aligned_bytes=2000 loaded=3 loaded_bytes=1800 reused_resident=0 evictions=1 evicted_bytes=900 capacity_retries=1",
            "I gm_synth: prepare_bench: case=eviction_commit_retire commit_ok=1 ack_ok=1 commit_us=4 retire_us=5 release_tail_survived=1 release_ok=1",
            "I gm_synth: prepare_bench: PASS used=2000 high_water=4500000 largest_free_span=4700000 resident_samples=3 total_prepare_successes=3 total_loaded_bytes=5700 total_evictions=1 total_evicted_bytes=900",
            "I gm_synth: PSRAM after Program Prepare: total=8388608 free=3640000 largest=3490000 minimum_free=3550000 bytes",
            "I gm_synth: INTERNAL_HEAP after Program Prepare: total=330000 free=240000 largest=112000 minimum_free=230000 bytes",
        ]
    )
    scope = "hardware" if physical else "qemu_regression"
    marker = 1 if physical else 0
    lines.append(
        f"I gm_synth: render_bench: START frames_per_block=64 sample_rate=44100 warmup_blocks=16 measured_blocks=128 budget_us=1451 timing_scope={scope} physical_timing_evidence={marker}"
    )
    for voices in VOICE_COUNTS:
        if voices == omit_voice:
            continue
        over = over_budget_from is not None and voices >= over_budget_from
        max_us = 1600 if over else 100 + voices * 20
        misses = 2 if over else 0
        lines.append(
            f"I gm_synth: render_bench: case voices={voices} blocks=128 min_us=20 avg_us=40 p50_us=42 p95_us=60 p99_us=70 max_us={max_us} deadline_misses={misses} clipped_samples=0 checksum={voices:04x}"
        )
    lines.extend(
        [
            f"I gm_synth: render_bench: PASS cases=8 final_refs=0 clock_faults=0 duration_clamps=0 timing_scope={scope} physical_timing_evidence={marker}",
            "I gm_synth: PSRAM after render benchmark: total=8388608 free=3630000 largest=3480000 minimum_free=3500000 bytes",
            "I gm_synth: INTERNAL_HEAP after render benchmark: total=330000 free=238000 largest=110000 minimum_free=225000 bytes",
            f"I gm_synth: M6_QUAL_MEMORY arena_capacity={arena} arena_used=2000 arena_high_water=4500000 arena_largest_free_span=4700000 resident_samples=3 allocation_failures=0 stale_reference_faults={stale_faults}",
            "I gm_synth: M6 bring-up core linked successfully; physical I2S output is intentionally not started in this target",
        ]
    )
    return "\n".join(lines)


class M6QualificationReportTests(unittest.TestCase):
    def test_passes_complete_physical_log(self):
        report = parse_log(make_log())
        self.assertTrue(report.passed, report.errors)
        self.assertEqual(report.highest_within_deadline_voice, 24)
        self.assertEqual(report.final_memory.arena_capacity, 4_718_592)
        markdown = render_markdown(report, title="M6 Test")
        self.assertIn("# M6 Test", markdown)
        self.assertIn("highest observed Voice case within the block deadline: `24`", markdown)
        self.assertIn("| 16 |", markdown)

    def test_rejects_qemu_timing_as_physical_evidence(self):
        report = parse_log(make_log(physical=False))
        self.assertFalse(report.passed)
        self.assertTrue(any("not marked as physical" in error for error in report.errors))

    def test_rejects_missing_voice_case(self):
        report = parse_log(make_log(omit_voice=18))
        self.assertFalse(report.passed)
        self.assertTrue(any(error.startswith("render:") for error in report.errors))

    def test_stretch_voice_deadline_miss_is_measured_not_global_failure(self):
        report = parse_log(make_log(over_budget_from=18))
        self.assertTrue(report.passed, report.errors)
        self.assertEqual(report.highest_within_deadline_voice, 16)

    def test_required_16_voice_deadline_miss_fails(self):
        report = parse_log(make_log(over_budget_from=16))
        self.assertFalse(report.passed)
        self.assertTrue(any("required Voice case 16" in error for error in report.errors))

    def test_rejects_stale_reference_fault(self):
        report = parse_log(make_log(stale_faults=1))
        self.assertFalse(report.passed)
        self.assertTrue(any("stale reference faults" in error for error in report.errors))

    def test_rejects_wrong_arena(self):
        report = parse_log(make_log(arena=4_194_304))
        self.assertFalse(report.passed)
        self.assertTrue(any("PCM arena mismatch" in error for error in report.errors))


if __name__ == "__main__":
    unittest.main()
