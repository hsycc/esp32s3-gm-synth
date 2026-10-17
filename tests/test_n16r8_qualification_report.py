from __future__ import annotations

import unittest

from tools.n16r8_qualification.report import parse_log, render_markdown


PASS_LOG = """
I (100) gm_synth: Detected physical PSRAM: 8388608 bytes (8.00 MiB)
I (110) gm_synth: PSRAM before arena: total=8388608 free=8385000 largest=8257536 minimum_free=8380000 bytes
I (111) gm_synth: INTERNAL_HEAP before arena: total=327680 free=280000 largest=190000 minimum_free=275000 bytes
I (112) gm_synth: Requesting PCM residency arena: 4718592 bytes (4.50 MiB)
I (120) gm_synth: Benchmark FATFS mounted: base=/sf2 partition=qemu_sf2
I (130) gm_synth: Storage benchmark VFS open: path=/sf2/test.sf2 file_bytes=5898214
I (140) gm_synth: Storage benchmark SF2 parsed: playable_samples=67
I (141) gm_synth: Storage benchmark source: path=/sf2/test.sf2 smpl_offset=544 smpl_bytes=5887930 benchmark_bytes=4194304
I (150) storage_bench: chunk=4096 bytes=4194304 chunks=1024 elapsed=700000 us throughput=5.714 MiB/s latency_us[min/avg/p50/p95/p99/max]=600/680/670/720/760/900
I (151) storage_bench: chunk=8192 bytes=4194304 chunks=512 elapsed=680000 us throughput=5.882 MiB/s latency_us[min/avg/p50/p95/p99/max]=1200/1320/1310/1380/1430/1550
I (152) storage_bench: chunk=16384 bytes=4194304 chunks=256 elapsed=660000 us throughput=6.061 MiB/s latency_us[min/avg/p50/p95/p99/max]=2450/2570/2560/2640/2710/2800
I (153) storage_bench: chunk=32768 bytes=4194304 chunks=128 elapsed=650000 us throughput=6.154 MiB/s latency_us[min/avg/p50/p95/p99/max]=4900/5050/5040/5150/5250/5400
I (154) storage_bench: chunk=65536 bytes=4194304 chunks=64 elapsed=640000 us throughput=6.250 MiB/s latency_us[min/avg/p50/p95/p99/max]=9800/10000/9980/10200/10500/10800
I (160) gm_synth: PSRAM after arena: total=8388608 free=3666000 largest=3600000 minimum_free=3650000 bytes
I (161) gm_synth: INTERNAL_HEAP after arena: total=327680 free=275000 largest=188000 minimum_free=270000 bytes
I (162) gm_synth: Residency ready: capacity=4718592 used=0 largest_free_span=4718592 free_spans=1
I (170) prepare_bench: start path=/sf2/test.sf2 samples=67 arena=4718592 A[count=5 aligned=540000] B[count=7 aligned=710000] read_chunk=32768
I (171) prepare_bench: case=cold_initial ok=1 elapsed_us=91000 dependencies=5 dependency_pcm_bytes=539990 dependency_aligned_bytes=540000 loaded=5 loaded_bytes=539990 reused_resident=0 evictions=0 evicted_bytes=0 capacity_retries=0
I (172) prepare_bench: case=warm_same_program ok=1 elapsed_us=120 dependencies=5 dependency_pcm_bytes=539990 dependency_aligned_bytes=540000 loaded=0 loaded_bytes=0 reused_resident=5 evictions=0 evicted_bytes=0 capacity_retries=0
I (173) prepare_bench: case=warm_commit_retire commit_ok=1 ack_ok=1 commit_us=2 retire_us=35
I (174) prepare_bench: case=prime_cache ok=1 elapsed_us=590000 loaded_samples=48 loaded_bytes=3900000 used=4440000 largest_free_span=278592 free_spans=3
I (175) prepare_bench: case=eviction_program_change ok=1 elapsed_us=145000 dependencies=7 dependency_pcm_bytes=709980 dependency_aligned_bytes=710000 loaded=7 loaded_bytes=709980 reused_resident=0 evictions=8 evicted_bytes=760000 capacity_retries=8
I (176) prepare_bench: case=eviction_commit_retire commit_ok=1 ack_ok=1 commit_us=3 retire_us=41 release_tail_survived=1 release_ok=1
I (177) prepare_bench: PASS used=4400000 high_water=4680000 largest_free_span=318592 resident_samples=52 total_prepare_successes=3 total_loaded_bytes=1249970 total_evictions=8 total_evicted_bytes=760000
I (180) gm_synth: PSRAM after Program Prepare: total=8388608 free=3660000 largest=3590000 minimum_free=3640000 bytes
I (181) gm_synth: INTERNAL_HEAP after Program Prepare: total=327680 free=268000 largest=184000 minimum_free=262000 bytes
I (190) gm_synth: render_bench: START frames_per_block=64 sample_rate=44100 warmup_blocks=16 measured_blocks=128 budget_us=1451 timing_scope=hardware physical_timing_evidence=1
I (191) gm_synth: render_bench: case voices=1 blocks=128 min_us=50 avg_us=60 p50_us=60 p95_us=70 p99_us=80 max_us=90 deadline_misses=0 clipped_samples=0 checksum=0001
I (192) gm_synth: render_bench: case voices=4 blocks=128 min_us=100 avg_us=120 p50_us=120 p95_us=140 p99_us=150 max_us=160 deadline_misses=0 clipped_samples=0 checksum=0004
I (193) gm_synth: render_bench: case voices=8 blocks=128 min_us=180 avg_us=220 p50_us=220 p95_us=250 p99_us=270 max_us=290 deadline_misses=0 clipped_samples=0 checksum=0008
I (194) gm_synth: render_bench: case voices=12 blocks=128 min_us=270 avg_us=330 p50_us=330 p95_us=380 p99_us=400 max_us=430 deadline_misses=0 clipped_samples=0 checksum=0012
I (195) gm_synth: render_bench: case voices=16 blocks=128 min_us=360 avg_us=440 p50_us=440 p95_us=500 p99_us=530 max_us=560 deadline_misses=0 clipped_samples=0 checksum=0016
I (196) gm_synth: render_bench: case voices=18 blocks=128 min_us=410 avg_us=500 p50_us=500 p95_us=570 p99_us=600 max_us=640 deadline_misses=0 clipped_samples=0 checksum=0018
I (197) gm_synth: render_bench: case voices=20 blocks=128 min_us=500 avg_us=700 p50_us=690 p95_us=900 p99_us=1200 max_us=1500 deadline_misses=1 clipped_samples=0 checksum=0020
I (198) gm_synth: render_bench: case voices=24 blocks=128 min_us=600 avg_us=800 p50_us=790 p95_us=1000 p99_us=1200 max_us=1400 deadline_misses=0 clipped_samples=0 checksum=0024
I (199) gm_synth: render_bench: PASS cases=8 final_refs=0 clock_faults=0 duration_clamps=0 timing_scope=hardware physical_timing_evidence=1
I (200) gm_synth: PSRAM after render benchmark: total=8388608 free=3655000 largest=3580000 minimum_free=3630000 bytes
I (201) gm_synth: INTERNAL_HEAP after render benchmark: total=327680 free=265000 largest=182000 minimum_free=260000 bytes
I (202) gm_synth: M6_QUAL_MEMORY arena_capacity=4718592 arena_used=4400000 arena_high_water=4680000 arena_largest_free_span=318592 resident_samples=52 allocation_failures=0 stale_reference_faults=0
I (203) gm_synth: M6 bring-up core linked successfully; physical I2S output is intentionally not started in this target
"""


class N16R8QualificationReportTests(unittest.TestCase):
    def test_physical_core_pass_and_candidate(self) -> None:
        report = parse_log(PASS_LOG)
        self.assertTrue(report.passed, report.errors)
        self.assertEqual(report.target_voices, 16)
        self.assertEqual(report.guaranteed_candidate_voices, 18)
        self.assertEqual(report.memory.capacity_bytes, 4718592)
        self.assertEqual(report.psram["after render benchmark"].free_bytes, 3655000)
        self.assertEqual(
            report.internal_heap["after render benchmark"].largest_free_block_bytes,
            182000,
        )
        self.assertEqual(len(report.render_cases), 8)

        markdown = render_markdown(report)
        self.assertIn("Status: **CORE PASS**", markdown)
        self.assertIn("contiguous zero-miss candidate: `18` voices", markdown)
        self.assertIn("| 20 | 900 | 1200 | 1500 | 1 | 0 | FAIL |", markdown)
        self.assertIn("Physical I2S output", markdown)

    def test_qemu_timing_is_rejected_as_physical_evidence(self) -> None:
        broken = PASS_LOG.replace(
            "timing_scope=hardware physical_timing_evidence=1",
            "timing_scope=qemu_regression physical_timing_evidence=0",
        )
        report = parse_log(broken)
        self.assertFalse(report.passed)
        self.assertTrue(
            any(
                "not marked as physical hardware evidence" in error
                for error in report.errors
            )
        )

    def test_sixteen_voice_deadline_miss_fails_target(self) -> None:
        broken = PASS_LOG.replace(
            "voices=16 blocks=128 min_us=360 avg_us=440 p50_us=440 p95_us=500 p99_us=530 max_us=560 deadline_misses=0",
            "voices=16 blocks=128 min_us=360 avg_us=440 p50_us=440 p95_us=500 p99_us=530 max_us=1600 deadline_misses=1",
        )
        report = parse_log(broken)
        self.assertFalse(report.passed)
        self.assertTrue(
            any("16-Voice target missed realtime gate" in error for error in report.errors)
        )
        self.assertEqual(report.guaranteed_candidate_voices, 12)

    def test_four_point_five_mib_arena_is_required(self) -> None:
        broken = PASS_LOG.replace(
            "Requesting PCM residency arena: 4718592 bytes",
            "Requesting PCM residency arena: 4194304 bytes",
        )
        report = parse_log(broken)
        self.assertFalse(report.passed)
        self.assertTrue(any("arena request mismatch" in error for error in report.errors))

    def test_memory_fault_is_rejected(self) -> None:
        broken = PASS_LOG.replace(
            "allocation_failures=0 stale_reference_faults=0",
            "allocation_failures=1 stale_reference_faults=2",
        )
        report = parse_log(broken)
        self.assertFalse(report.passed)
        self.assertTrue(any("allocation failures=1" in error for error in report.errors))
        self.assertTrue(any("stale residency references=2" in error for error in report.errors))

    def test_panic_is_rejected(self) -> None:
        report = parse_log(PASS_LOG + "\nGuru Meditation Error: Core 0 panic'ed\n")
        self.assertFalse(report.passed)
        self.assertTrue(any("Guru Meditation" in error for error in report.errors))


if __name__ == "__main__":
    unittest.main()
