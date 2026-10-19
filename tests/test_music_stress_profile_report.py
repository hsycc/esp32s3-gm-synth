import unittest

from tools.music_stress_profile.report import parse_log, render_markdown


SUMMARY = (
    "I gm_synth: i2s_worst: SUMMARY deadline_us=1451 deadline_candidates=209 "
    "retained=2 slowest_render_us=2186 retained_floor_us=1998"
)
BLOCK_1 = (
    "I gm_synth: i2s_worst: BLOCK rank=1 stream_block=8129 frame=520256 render_us=2186 "
    "events=4 note_on=0 note_off=0 cc=1 program=1 pitch=1 pressure=1 gm_reset=0 "
    "event_channels=0x0007 sustain=0x0004 active=27 releasing=11 lowpass=18 vibrato=8 "
    "mod_lfo=12 mod_env=9 fx_send=24 realtime_control=6 fast_release_tails=2 "
    "steal_tails=1 exclusive_tails=0 voice_channels=0x02ff"
)
BLOCK_2 = (
    "I gm_synth: i2s_worst: BLOCK rank=2 stream_block=6200 frame=396800 render_us=1998 "
    "events=0 note_on=0 note_off=0 cc=0 program=0 pitch=0 pressure=0 gm_reset=0 "
    "event_channels=0x0000 sustain=0x0001 active=24 releasing=12 lowpass=16 vibrato=6 "
    "mod_lfo=10 mod_env=8 fx_send=20 realtime_control=5 fast_release_tails=1 "
    "steal_tails=0 exclusive_tails=0 voice_channels=0x00ff"
)


class MusicStressProfileReportTests(unittest.TestCase):
    def test_valid_top_n_profile(self):
        report = parse_log("\n".join((SUMMARY, BLOCK_1, BLOCK_2)))
        self.assertTrue(report.passed, report.errors)
        self.assertEqual(report.deadline_us, 1451)
        self.assertEqual(report.deadline_candidates, 209)
        self.assertEqual(report.blocks[0].active_voices, 27)
        self.assertEqual(report.blocks[0].releasing_voices, 11)
        self.assertEqual(report.blocks[0].program_change, 1)
        self.assertEqual(report.blocks[0].sustain_channel_mask, 0x0004)
        markdown = render_markdown(report)
        self.assertIn("2186", markdown)
        self.assertIn("0x0004", markdown)

    def test_missing_summary_is_invalid(self):
        report = parse_log(BLOCK_1)
        self.assertFalse(report.passed)
        self.assertIn("missing i2s_worst SUMMARY", report.errors)

    def test_retained_count_mismatch_is_invalid(self):
        bad_summary = SUMMARY.replace("retained=2", "retained=3")
        report = parse_log("\n".join((bad_summary, BLOCK_1, BLOCK_2)))
        self.assertFalse(report.passed)
        self.assertTrue(any("retained count mismatch" in error for error in report.errors))

    def test_deadline_candidate_below_retained_is_invalid(self):
        bad_summary = SUMMARY.replace("deadline_candidates=209", "deadline_candidates=1")
        report = parse_log("\n".join((bad_summary, BLOCK_1, BLOCK_2)))
        self.assertFalse(report.passed)
        self.assertTrue(any("deadline candidates below retained" in error for error in report.errors))

    def test_rank_and_sort_order_are_validated(self):
        rank2_fast = BLOCK_2.replace("render_us=1998", "render_us=2300")
        report = parse_log("\n".join((SUMMARY, BLOCK_1, rank2_fast)))
        self.assertFalse(report.passed)
        self.assertTrue(any("not sorted" in error for error in report.errors))

    def test_feature_count_cannot_exceed_active_voices(self):
        bad_block = BLOCK_1.replace("active=27", "active=5")
        report = parse_log("\n".join((SUMMARY, bad_block, BLOCK_2)))
        self.assertFalse(report.passed)
        self.assertTrue(any("voices" in error and "exceed active" in error for error in report.errors))

    def test_event_subtypes_must_sum_to_total(self):
        bad_block = BLOCK_1.replace("events=4", "events=5")
        report = parse_log("\n".join((SUMMARY, bad_block, BLOCK_2)))
        self.assertFalse(report.passed)
        self.assertTrue(any("event subtype sum" in error for error in report.errors))

    def test_empty_profile_is_valid_when_no_deadline_miss(self):
        log = (
            "I gm_synth: i2s_worst: SUMMARY deadline_us=1451 deadline_candidates=0 "
            "retained=0 slowest_render_us=0 retained_floor_us=0"
        )
        report = parse_log(log)
        self.assertTrue(report.passed, report.errors)
        self.assertEqual(report.blocks, ())


if __name__ == "__main__":
    unittest.main()
