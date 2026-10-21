"""Regression checks for three-run M7 physical log qualification parser."""
import tempfile
import unittest
from pathlib import Path

from tools.m7_physical_three_run import parse_log, report


def sample_log(version="abcdef0", gate="0x00000000", cc=3):
    parts=[f"I (682) app_init: App version:      {version}"]
    for _ in range(2):
        parts += [
            "I gm_synth: i2s_qual: RESULT blocks=131072 render_us[min/avg/max]=130/743/2006 render_deadline_misses=5",
            "I gm_synth: i2s_qual: REALTIME_HEALTH render_soft_overruns=5 max_backlog_us=559 final_backlog_us=0 min_dma_margin_us=11051 backlog_healthy=1",
            f"I gm_synth: i2s_qual: HARD_GATE failure_flags={gate}",
            "I gm_synth: i2s_qual: PATH startup_prime_blocks=16 startup_prime_send_queue_overflows=1 active_synth_send_queue_overflows=0"]
    parts += [
        "I gm_synth: i2s_worst: STAGES rank=1 stream_block=10 stage_valid=0 setup_prepare_us=0 event_apply_us=0 voice_render_us=0 effects_finalize_us=0",
        "I gm_synth: i2s_worst: BLOCK rank=1 frame=1024 render_us=2100 events=0 note_on=0",
        "I gm_synth: i2s_worst: STAGES rank=2 stream_block=100 stage_valid=1 setup_prepare_us=7 event_apply_us=920 voice_render_us=850 effects_finalize_us=100",
        "I gm_synth: i2s_worst: BLOCK rank=2 frame=3775488 render_us=2006 events=24 note_on=24",
        "I gm_synth: music_stress: NOTE_ON_BURST frame=3775488 notes=24 zone_resolve_total_us=360 voice_start_total_us=530 zone_resolve_max_event_us=27 voice_start_max_event_us=35 timing_gate=diagnostic_only",
        f"I gm_synth: music_stress: CC21_FILTER channel=5 filter_changes={cc}",
        "I gm_synth: music_stress: RESULT max_stable_polyphony=24 PASS",
        "I gm_synth: physical_polyphony: RESULT max_stable_voices=24 PASS",
        "I main_task: Returned from app_main()",
    ]
    return "\n".join(parts)


class M7PhysicalCompareTests(unittest.TestCase):
    def parse(self,content):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d)/"physical.log"
            p.write_text(content)
            return parse_log(p)

    def test_qualifies_burst_even_when_worst_rank_not_burst(self):
        r=self.parse(sample_log())
        self.assertTrue(all(r["checks"].values()))
        self.assertEqual(r["stage_event_apply_us"],920)
        self.assertEqual(r["burst_zone_resolve_total_us"],360)
        self.assertEqual(r["render_max_us"],2006)

    def test_zero_soft_overruns_need_no_retained_worst_block(self):
        no_worst="\n".join(line for line in sample_log().splitlines()
                           if "i2s_worst:" not in line)
        r=self.parse(no_worst)
        self.assertTrue(all(r["checks"].values()))
        self.assertFalse(r["burst_stage_retained"])

    def test_hard_gate_and_filter_change_are_not_softened(self):
        r=self.parse(sample_log(gate="0x00000020",cc=2))
        self.assertFalse(r["checks"]["hard_gate"])
        self.assertFalse(r["checks"]["cc21_sweep"])

    def test_missing_final_marker_is_incomplete(self):
        incomplete=sample_log().replace("I main_task: Returned from app_main()","")
        with self.assertRaisesRegex(ValueError,"incomplete"):
            self.parse(incomplete)

    def test_wdt_is_rejected(self):
        with self.assertRaisesRegex(ValueError,"crash/WDT"):
            self.parse(sample_log()+"\ntask_wdt: Task watchdog got triggered")

    def test_comparison_requires_same_firmware(self):
        a=self.parse(sample_log())
        b=self.parse(sample_log(version="1234567"))
        with self.assertRaisesRegex(ValueError,"differs"):
            report([a,a,b])
        with self.assertRaisesRegex(ValueError,"three"):
            report([a,b])


if __name__=="__main__":
    unittest.main()
