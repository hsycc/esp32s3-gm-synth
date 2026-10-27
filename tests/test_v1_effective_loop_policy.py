import io
import json
import math
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

from tools.v1_loop_policy import analyze, choose, context_rejections
from tools.v1_effective_loop_survey import identity, read_manifest, validate_zone, survey

CTX = dict(scope="prepared_zone", loop_mode="continuous", sample_type=1,
           noteon_address_modulated=False)


def metric(fade, seam, **kw):
    row = dict(fade_frames=fade, seam_step=seam, near_max=9000 if fade else 10000,
               near_rms=900 if fade else 1000, spectral_distance=0.1,
               lag_shift=0, period_change_fraction=0)
    row.update(kw)
    return row


class PolicyTests(unittest.TestCase):
    def test_risky_best_chooses_next_candidate(self):
        out = choose([metric(0, 10000), metric(16, 1500),
                      metric(32, 50, spectral_distance=.9)], 10000, CTX)
        self.assertEqual(out["seam_best_fade_frames"], 32)
        self.assertEqual(out["selected_fade_frames"], 16)
        self.assertIn("HIGH_LOCAL_SPECTRAL_DISTANCE", out["candidates"][2]["rejection_reasons"])
        self.assertFalse(out["runtime_approved"])
        self.assertEqual(out["runtime_fade_frames"], 0)

    def test_all_risky_keeps_direct_review(self):
        out = choose([metric(0, 10000), metric(32, 20, lag_shift=1)], 10000, CTX)
        self.assertEqual(out["decision"], "DIRECT_REVIEW")
        self.assertEqual(out["selected_fade_frames"], 0)

    def test_same_candidates_reorder_stable_shorter_tie(self):
        a = [metric(0, 10000), metric(32, 100), metric(16, 100)]
        self.assertEqual(choose(a, 10000, CTX)["selected_fade_frames"], 16)
        self.assertEqual(choose(list(reversed(a)), 10000, CTX)["selected_fade_frames"], 16)

    def test_exact_period_boundary_and_unrounded_geometry(self):
        a = [metric(0, 10000), metric(16, 100)]
        self.assertEqual(choose(a, 1600, CTX)["selected_fade_frames"], 0)
        self.assertEqual(choose(a, 1601, CTX)["selected_fade_frames"], 16)

    def test_undefined_or_nonfinite_metrics_fail_closed(self):
        for val in (None, float("nan"), float("inf")):
            out = choose([metric(0, 10000), metric(16, 100, spectral_distance=val)], 10000, CTX)
            self.assertEqual(out["selected_fade_frames"], 0)
        out = choose([metric(0, 10000), metric(16, 100, lag_shift=None)], 10000, CTX)
        self.assertEqual(out["selected_fade_frames"], 0)

    def test_none_raw_stereo_dynamic_never_auto_selected(self):
        for ctx in ({**CTX, "loop_mode": "none"}, {**CTX, "scope": "raw_shdr"},
                    {**CTX, "sample_type": 4}, {**CTX, "noteon_address_modulated": True}):
            out = choose([metric(0, 10000), metric(16, 100)], 10000, ctx)
            self.assertEqual(out["decision"], "DIRECT_CONTEXT_BLOCKED")
            self.assertEqual(out["selected_fade_frames"], 0)

    def test_until_release_retains_own_context(self):
        self.assertFalse(context_rejections({**CTX, "loop_mode": "until_release"}))

    def test_small_original_seam_explains_direct(self):
        out = choose([metric(0, 1500), metric(16, 0)], 10000, CTX)
        self.assertEqual(out["decision"], "DIRECT_NO_BENEFIT")
        self.assertIn("DIRECT_SEAM_BELOW_REVIEW_FLOOR", out["candidates"][1]["rejection_reasons"])

    def test_new_nearby_spike_rejects(self):
        out = choose([metric(0, 10000), metric(16, 50, near_max=20000)], 10000, CTX)
        self.assertIn("NEAR_SEAM_PEAK_WORSE", out["candidates"][1]["rejection_reasons"])
        self.assertEqual(out["selected_fade_frames"], 0)

    def test_symmetric_window_sees_post_wrap_spike(self):
        pcm = [0] * 4096
        pcm[20] = 25000
        r = analyze(pcm, CTX)
        d = r["candidates"][0]
        self.assertEqual(d["legacy_one_sided_near_max"], 0)
        self.assertEqual(d["near_max"], 25000)

    def test_silence_preserved_and_all_candidates_retained(self):
        r = analyze([0]*4096, CTX)
        self.assertEqual(r["selected_fade_frames"], 0)
        self.assertEqual([c["fade_frames"] for c in r["candidates"]], [0, 16, 32, 64])
        self.assertTrue(all("rejection_reasons" in c for c in r["candidates"]))

    def test_intentional_short_ramp_not_repaired_by_seam_only(self):
        pcm = [round(-16000 + i*32000/799) for i in range(800)]
        r = analyze(pcm, CTX)
        self.assertEqual(r["selected_fade_frames"], 0)
        self.assertTrue(all("PERIOD_CHANGE_GE_REVIEW_LIMIT" in c["risk_rejections"]
                            for c in r["candidates"][1:]))

    def test_short_loop_and_invalid_pcm(self):
        self.assertEqual(len(analyze([0]*16, CTX)["candidates"]), 1)
        for pcm in ([0, 1], [0]*10 + [40000], [0]*10 + [float("nan")]):
            with self.assertRaises(ValueError):
                analyze(pcm, CTX)

    def test_bad_candidate_shapes(self):
        for candidates in ([], [metric(16, 1)], [metric(0, 1), metric(0, 1)],
                           [metric(0, 100), metric(2048, 1)]):
            with self.assertRaises(ValueError):
                choose(candidates, 4096, CTX)


class ManifestTests(unittest.TestCase):
    def zone(self):
        return dict(CTX, sample_id=0, sample_link=0, rate=44100, source_start=100,
                    source_end=1100, start=100, end=1100, loop_start=200, loop_end=1000,
                    bank=0, program=0, zone_index=0, preset_index=0)

    def test_key_includes_asset_region_mode_and_policy(self):
        z = self.zone()
        k = identity(z, "a"*64)
        self.assertNotEqual(k, identity(z, "b"*64))
        self.assertNotEqual(k, identity({**z, "loop_start": 201}, "a"*64))
        self.assertNotEqual(k, identity({**z, "loop_mode": "until_release"}, "a"*64))
        # Bank/program reference alone does not change identical PCM geometry.
        self.assertEqual(k, identity({**z, "program": 2}, "a"*64))

    def test_region_bounds_and_nonloop_sentinel(self):
        z = self.zone()
        validate_zone(z, {"smpl_points": 1200})
        validate_zone({**z, "loop_mode": "none", "loop_start": 0, "loop_end": 0}, {"smpl_points": 1200})
        with self.assertRaises(ValueError):
            validate_zone({**z, "loop_end": 1201}, {"smpl_points": 1200})

    def test_cli_module_and_direct_help(self):
        root = Path(__file__).resolve().parents[1]
        for command in ([sys.executable, "-m", "tools.v1_effective_loop_survey", "--help"],
                        [sys.executable, "tools/v1_effective_loop_survey.py", "--help"]):
            r = subprocess.run(command, cwd=root, text=True, capture_output=True, check=True)
            self.assertIn("--exporter", r.stdout)

    def test_missing_summary_and_wrong_producer_rejected(self):
        for records in ([dict(kind="meta", schema=1, producer="Sf2PlaybackProgramPreparer")],
                        [dict(kind="meta", schema=1, producer="raw_shdr")]):
            with self.assertRaises(ValueError):
                read_manifest(io.StringIO("\n".join(json.dumps(r) for r in records)))

    def test_terminal_manifest_counts_checked(self):
        meta = dict(kind="meta", schema=1, producer="Sf2PlaybackProgramPreparer")
        summary = dict(kind="summary", zones=0, rejected_programs=0)
        data = "\n".join(json.dumps(r) for r in (meta, summary))
        self.assertEqual(read_manifest(io.StringIO(data))[1], [])
        with self.assertRaises(ValueError):
            read_manifest(io.StringIO(data + "\n" + json.dumps(summary)))


def synthetic_bank(path):
    """One source sample in four instrument Zones with different loop semantics."""
    def chunk(tag, payload):
        return tag + struct.pack("<I", len(payload)) + payload + (b"\0" if len(payload)%2 else b"")
    def gens(items):
        return b"".join(struct.pack("<HH", op, n & 65535) for op, n in items)
    zone_gens = [[(54, 0), (53, 0)],
                 [(0, 5), (1, -5), (2, 7), (3, -9), (54, 1), (53, 0)],
                 [(54, 3), (53, 0)], [(0, 150), (54, 1), (53, 0)]]
    flat, bags = [], []
    for g in zone_gens:
        bags.append((len(flat), 0)); flat += g
    bags.append((len(flat), 0))
    phdr = struct.pack("<20sHHHIII", b"Fixture", 0, 0, 0, 0, 0, 0) + struct.pack("<20sHHHIII", b"EOP", 0, 0, 1, 0, 0, 0)
    inst = struct.pack("<20sH", b"Test", 0) + struct.pack("<20sH", b"EOI", 4)
    shdr = struct.pack("<20sIIIIIBbHH", b"Wave", 100, 1100, 200, 1000, 44100, 60, 0, 0, 1)
    shdr += struct.pack("<20sIIIIIBbHH", b"EOS", 0, 0, 0, 0, 0, 0, 0, 0, 0)
    tables = [(b"phdr", phdr), (b"pbag", struct.pack("<HHHH", 0, 0, 1, 0)),
              (b"pmod", bytes(10)), (b"pgen", gens([(41, 0), (0, 0)])),
              (b"inst", inst), (b"ibag", b"".join(struct.pack("<HH", *b) for b in bags)),
              (b"imod", bytes(10)), (b"igen", gens(flat+[(0, 0)])), (b"shdr", shdr)]
    sdta = chunk(b"LIST", b"sdta" + chunk(b"smpl", bytes(2400)))
    pdta = chunk(b"LIST", b"pdta" + b"".join(chunk(t, p) for t,p in tables))
    path.write_bytes(chunk(b"RIFF", b"sfbk" + sdta + pdta))


@unittest.skipUnless(os.environ.get("V1_LOOP_EXPORTER"), "compiled production exporter runs in dedicated CI")
class NativeExporterTests(unittest.TestCase):
    def test_production_modes_offsets_clamp_and_name(self):
        with tempfile.TemporaryDirectory() as td:
            sf2 = Path(td)/"fixture.sf2"
            synthetic_bank(sf2)
            run = subprocess.run([os.environ["V1_LOOP_EXPORTER"], str(sf2)],
                                 capture_output=True, text=True, check=True)
            meta, zones, rejected, summary = read_manifest(io.StringIO(run.stdout))
            self.assertFalse(rejected)
            self.assertEqual(len(zones), 4)
            self.assertEqual(zones[0]["loop_mode"], "none")
            self.assertEqual((zones[1]["start"], zones[1]["end"], zones[1]["loop_start"], zones[1]["loop_end"]), (105, 1095, 207, 991))
            self.assertEqual(zones[2]["loop_mode"], "until_release")
            self.assertEqual((zones[3]["start"], zones[3]["loop_start"]), (250, 250))
            self.assertTrue(all(z["name"] == "Wave" for z in zones))
            self.assertEqual(len({identity(z, "a"*64) for z in zones}), 4)

    def test_end_to_end_prepared_zone_survey(self):
        with tempfile.TemporaryDirectory() as td:
            sf2 = Path(td)/"fixture.sf2"
            synthetic_bank(sf2)
            r = survey(sf2, Path(os.environ["V1_LOOP_EXPORTER"]), limit=128)
            self.assertEqual(r["unique_geometries"], 4)
            self.assertEqual(r["analyzed_geometries"], 3)
            self.assertEqual(r["decision_counts"], {"DIRECT_CONTEXT_BLOCKED": 1, "DIRECT_NO_BENEFIT": 3})
            self.assertTrue(all(x["runtime_fade_frames"] == 0 for x in r["rows"]))

    def test_malformed_bank_fails(self):
        with tempfile.TemporaryDirectory() as td:
            p = Path(td)/"bad.sf2"
            p.write_bytes(b"bad")
            r = subprocess.run([os.environ["V1_LOOP_EXPORTER"], str(p)], capture_output=True)
            self.assertNotEqual(r.returncode, 0)


if __name__ == "__main__":
    unittest.main()
