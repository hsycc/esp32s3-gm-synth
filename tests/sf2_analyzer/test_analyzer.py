import json
import struct
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from tools.sf2_analyzer import Sf2Error, analyze


def chunk(chunk_id: bytes, payload: bytes) -> bytes:
    body = chunk_id + struct.pack("<I", len(payload)) + payload
    if len(payload) & 1:
        body += b"\x00"
    return body


def list_chunk(list_type: bytes, children: bytes) -> bytes:
    return chunk(b"LIST", list_type + children)


def fixed_name(name: str) -> bytes:
    raw = name.encode("ascii")[:20]
    return raw + b"\x00" * (20 - len(raw))


def phdr(name: str, preset: int, bank: int, bag_index: int) -> bytes:
    return fixed_name(name) + struct.pack("<HHHIII", preset, bank, bag_index, 0, 0, 0)


def inst(name: str, bag_index: int) -> bytes:
    return fixed_name(name) + struct.pack("<H", bag_index)


def shdr(name: str, start: int, end: int, link: int = 0, sample_type: int = 1, rate: int = 44100) -> bytes:
    return fixed_name(name) + struct.pack("<IIIIIBbHH", start, end, start, end, rate, 60, 0, link, sample_type)


def gen(oper: int, amount: int) -> bytes:
    return struct.pack("<HH", oper, amount)


def bag(gen_index: int, mod_index: int = 0) -> bytes:
    return struct.pack("<HH", gen_index, mod_index)


def mod(src=0, dest=0, amount=0, amount_src=0, transform=0) -> bytes:
    return struct.pack("<HHhHH", src, dest, amount, amount_src, transform)


def make_sf2(two_zones=False, modulators=False, stereo=False) -> bytes:
    pgen = gen(41, 0)
    pbag = bag(0, 0) + bag(1, 1 if modulators else 0)
    pmod = (mod(1, 48, -120) + mod()) if modulators else mod()

    if two_zones:
        igen = gen(53, 0) + gen(53, 1)
        ibag = bag(0, 0) + bag(1, 1 if modulators else 0) + bag(2, 1 if modulators else 0)
        inst_table = inst("Inst", 0) + inst("EOI", 2)
    else:
        igen = gen(53, 0)
        ibag = bag(0, 0) + bag(1, 1 if modulators else 0)
        inst_table = inst("Inst", 0) + inst("EOI", 1)

    imod = (mod(2, 8, 100) + mod()) if modulators else mod()

    sample0_type = 4 if stereo else 1
    sample0_link = 1 if stereo else 0
    sample_table = shdr("S0", 0, 100, sample0_link, sample0_type, 48000 if stereo else 44100)
    if two_zones or stereo:
        sample1_type = 2 if stereo else 1
        sample1_link = 0
        sample_table += shdr("S1", 100, 180, sample1_link, sample1_type)
    sample_table += shdr("EOS", 180, 180)

    pdta = b"".join([
        chunk(b"phdr", phdr("Preset", 0, 0, 0) + phdr("EOP", 0, 0, 1)),
        chunk(b"pbag", pbag), chunk(b"pmod", pmod), chunk(b"pgen", pgen),
        chunk(b"inst", inst_table), chunk(b"ibag", ibag), chunk(b"imod", imod),
        chunk(b"igen", igen), chunk(b"shdr", sample_table),
    ])
    sample_points = 180 if (two_zones or stereo) else 100
    sdta = chunk(b"smpl", b"\x00\x00" * sample_points)
    info = chunk(b"ifil", struct.pack("<HH", 2, 4))
    body = b"sfbk" + list_chunk(b"INFO", info) + list_chunk(b"sdta", sdta) + list_chunk(b"pdta", pdta)
    return b"RIFF" + struct.pack("<I", len(body)) + body


class AnalyzerTests(unittest.TestCase):
    def write_sf2(self, payload: bytes) -> Path:
        tmp = tempfile.NamedTemporaryFile(suffix=".sf2", delete=False)
        tmp.write(payload); tmp.close()
        self.addCleanup(lambda: Path(tmp.name).unlink(missing_ok=True))
        return Path(tmp.name)

    def test_minimal_bank(self):
        report = analyze(self.write_sf2(make_sf2()), psram_bytes=1024, reserve_bytes=128)
        self.assertEqual(report["counts"]["presets"], 1)
        self.assertEqual(report["counts"]["instruments"], 1)
        self.assertEqual(report["counts"]["samples"], 1)
        self.assertEqual(report["sdta"]["smpl_payload_bytes"], 200)
        self.assertEqual(report["banks"]["program_counts"], {"0": 1})
        self.assertEqual(report["samples"]["sample_rate_distribution"], {"44100": 1})
        program = report["programs"][0]
        self.assertEqual(program["unique_sample_count"], 1)
        self.assertEqual(program["pcm_bytes"], 200)
        self.assertEqual(program["max_overlapping_zones"], 1)
        self.assertTrue(report["residency"]["fits_all_samples"])
        self.assertEqual(len(report["file"]["sha256"]), 64)
        self.assertEqual(
            report["compatibility"]["features"]["generator_table_parsing"],
            "implemented",
        )
        self.assertEqual(
            report["compatibility"]["features"]["default_sf2_modulators"],
            "planned_m2_5",
        )

    def test_overlap_modulators_and_stereo_links(self):
        report = analyze(self.write_sf2(make_sf2(two_zones=True, modulators=True, stereo=True)))
        program = report["programs"][0]
        self.assertEqual(program["resolved_zone_count"], 2)
        self.assertEqual(program["max_overlapping_zones"], 2)
        self.assertEqual(program["unique_sample_count"], 2)
        self.assertEqual(report["modulators"]["pmod_nonzero_count"], 1)
        self.assertEqual(report["modulators"]["imod_nonzero_count"], 1)
        self.assertEqual(report["modulators"]["pmod_amount_nonzero_count"], 1)
        self.assertEqual(report["modulators"]["imod_amount_nonzero_count"], 1)
        self.assertEqual(report["samples"]["stereo_side_sample_count"], 2)
        self.assertEqual(report["samples"]["linked_sample_count"], 2)
        self.assertEqual(report["samples"]["sample_rate_distribution"], {"44100": 1, "48000": 1})
        compat = report["compatibility"]
        self.assertIn("custom_pmod_imod_semantics", compat["planned_features_exercised"])
        self.assertIn("stereo_linked_sample_runtime", compat["planned_features_exercised"])
        self.assertEqual(compat["modulators"]["pmod_effective_record_count"], 1)
        self.assertEqual(compat["modulators"]["imod_effective_record_count"], 1)

    def test_json_serializable(self):
        text = json.dumps(analyze(self.write_sf2(make_sf2())))
        self.assertIn('"schema_version": 3', text)
        self.assertIn('"compatibility"', text)

    def test_rejects_non_sf2(self):
        with self.assertRaises(Sf2Error):
            analyze(self.write_sf2(b"not a soundfont"))


if __name__ == "__main__":
    unittest.main()
