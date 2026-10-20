import unittest

from tools.m7_release.physical_report import analyze, markdown


def passing_log(blocks: int = 131072) -> str:
    return f"""
I (1) gm_synth: physical_product_envelope: RESULT normal_full_fx_max=24 nominal_candidate=24 pathological_heavy_max=8 heavy_first_unstable=12 heavy_boundary_reached=1 heavy_reason_flags=0x00000001
I (2) gm_synth: i2s_qual: PATH startup_prime_blocks=16 startup_sent_callbacks=16 startup_prime_send_queue_overflows=1 startup_overflow_gate=diagnostic_only tone_hz=441 tone_blocks=768 tone_peak=8192 tone_sent_callbacks=768 tone_send_queue_overflows=0 synth_warmup_callbacks=64 synth_warmup_send_queue_overflows=0 synth_sent_callbacks={blocks} synth_send_queue_overflows=0 active_synth_send_queue_overflows=0 shutdown_tail_callbacks=8 shutdown_tail_overflows=0
I (3) gm_synth: i2s_qual: RESULT blocks={blocks} render_us[min/avg/max]=120/760/1896 producer_work_us[min/avg/max]=123/764/1899 diagnostic_overhead_us[min/avg/max]=0/3/15 write_wait_us[min/avg/max]=5/680/1300 cycle_us[min/avg/max]=900/1441/2070 render_deadline_misses=5 render_deadline_gate=soft cycle_over_budget=20 cycle_budget_gate=soft producer_stalls_over_dma_window=0 write_timeouts=0 partial_writes=0 pcm_peak_abs=12000 pcm_avg_abs=1400 pcm_nonzero_samples=100000 pcm_samples=200000 clipped_samples=0 stack_high_water_bytes=2500 checksum=0123456789abcdef
I (4) gm_synth: i2s_qual: REALTIME_HEALTH block_budget_us=1451 dma_window_us=11610 render_soft_overruns=5 producer_work_soft_overruns=6 avg_producer_work_us=764 max_consecutive_work_overruns=2 max_backlog_us=448 max_backlog_blocks=1 max_backlog_recovery_blocks=2 final_backlog_us=0 min_dma_margin_us=11162 backlog_exceeded_dma_window=0 backlog_healthy=1
I (5) gm_synth: i2s_qual: HARD_GATE failure_flags=0x00000000 producer_stalls=0 write_timeouts=0 partial_writes=0 tone_queue_overflows=0 active_synth_queue_overflows=0 backlog_healthy=1 clipped_samples=0 stack_headroom_ok=1 transient_render_overruns=5 transient_cycle_overruns=20
I (6) gm_synth: i2s_qual: PASS
I (7) gm_synth: music_stress: RESULT max_stable_polyphony=24 peak_dynamic_voices=24 queue_remaining=0 queue_rejected_full=0 queue_rejected_order=0 PASS
I (8) gm_synth: effects_bench: PASS final_refs=0
""".strip()


class M7PhysicalReleaseReportTests(unittest.TestCase):
    def test_long_run_passes_with_recovered_backlog_and_clean_hard_gates(self) -> None:
        report = analyze(passing_log())
        self.assertTrue(report.passed, report.errors)
        self.assertEqual(report.measured_blocks, 131072)
        self.assertEqual(report.max_backlog_us, 448)
        self.assertEqual(report.final_backlog_us, 0)
        self.assertEqual(report.min_dma_margin_us, 11162)
        self.assertEqual(report.hard_failure_flags, 0)
        self.assertEqual(report.normal_full_fx_max, 24)
        self.assertEqual(report.pathological_heavy_max, 8)
        self.assertIn("**Result: PASS**", markdown(report))

    def test_hard_gate_failure_and_active_queue_overflow_are_blocking(self) -> None:
        text = passing_log().replace(
            "failure_flags=0x00000000 producer_stalls=0 write_timeouts=0 partial_writes=0 tone_queue_overflows=0 active_synth_queue_overflows=0",
            "failure_flags=0x00000010 producer_stalls=0 write_timeouts=0 partial_writes=0 tone_queue_overflows=0 active_synth_queue_overflows=1",
        )
        report = analyze(text)
        self.assertFalse(report.passed)
        self.assertIn("hard failure flags nonzero: 0x00000010", report.errors)
        self.assertIn("active synth queue overflows: 1", report.errors)

    def test_unrecovered_or_dma_exceeding_backlog_is_blocking(self) -> None:
        text = passing_log().replace(
            "final_backlog_us=0 min_dma_margin_us=11162 backlog_exceeded_dma_window=0 backlog_healthy=1",
            "final_backlog_us=400 min_dma_margin_us=0 backlog_exceeded_dma_window=1 backlog_healthy=0",
        )
        report = analyze(text)
        self.assertFalse(report.passed)
        self.assertIn("final backlog not recovered: 400 us", report.errors)
        self.assertIn("backlog exceeded DMA window", report.errors)
        self.assertIn("realtime backlog health gate did not pass", report.errors)

    def test_missing_telemetry_and_short_measurement_are_blocking(self) -> None:
        text = passing_log(blocks=16384)
        text = "\n".join(
            line for line in text.splitlines() if "i2s_qual: HARD_GATE" not in line
        )
        report = analyze(text)
        self.assertFalse(report.passed)
        self.assertIn("missing HARD_GATE telemetry", report.errors)
        self.assertIn("measured blocks 16384 below required 131072", report.errors)

    def test_fatal_runtime_marker_is_blocking_even_with_pass_telemetry(self) -> None:
        report = analyze(passing_log() + "\nGuru Meditation Error: Core 1 panic'ed")
        self.assertFalse(report.passed)
        self.assertIn("fatal marker present: Guru Meditation Error", report.errors)
        self.assertIn("fatal marker present: panic'ed", report.errors)


if __name__ == "__main__":
    unittest.main()
