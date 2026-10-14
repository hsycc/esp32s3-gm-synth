import struct
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from tools.sf2_analyzer import Sf2, resolve_note, resolve_preset_zones


def chunk(cid: bytes, payload: bytes) -> bytes:
    out = cid + struct.pack("<I", len(payload)) + payload
    return out + (b"\0" if len(payload) & 1 else b"")


def list_chunk(kind: bytes, payload: bytes) -> bytes:
    return chunk(b"LIST", kind + payload)


def name20(name: str) -> bytes:
    raw = name.encode("ascii")[:20]
    return raw + b"\0" * (20 - len(raw))


def phdr(name: str, preset: int, bank: int, bag_index: int) -> bytes:
    return name20(name) + struct.pack("<HHHIII", preset, bank, bag_index, 0, 0, 0)


def inst(name: str, bag_index: int) -> bytes:
    return name20(name) + struct.pack("<H", bag_index)


def bag(gen_index: int, mod_index: int = 0) -> bytes:
    return struct.pack("<HH", gen_index, mod_index)


def gen(oper: int, amount: int) -> bytes:
    return struct.pack("<HH", oper, amount & 0xFFFF)


def key_range(lo: int, hi: int) -> int:
    return lo | (hi << 8)


def shdr(name: str, start: int, end: int, loop_start: int, loop_end: int) -> bytes:
    return name20(name) + struct.pack(
        "<IIIIIBbHH", start, end, loop_start, loop_end, 44100, 60, -2, 0, 1
    )


def hierarchy_sf2() -> bytes:
    # Preset global: coarse tune +2, fine tune +3, key range 30..110.
    # Preset local: fine tune +5 supersedes +3, key range 20..100,
    # and an invalid preset-level sampleModes which must be ignored.
    pgen = b"".join(
        [
            gen(51, 2),
            gen(52, 3),
            gen(43, key_range(30, 110)),
            gen(52, 5),
            gen(43, key_range(20, 100)),
            gen(54, 3),
            gen(41, 0),
            gen(0, 0),
        ]
    )
    pbag = bag(0) + bag(3) + bag(7)

    # Instrument global: attenuation 100, pan 50, fine tune 10,
    # key range 35..90.
    # Instrument local overrides attenuation/fine tune, narrows key range,
    # enables looping, overrides root key and offsets sample/loop start.
    igen = b"".join(
        [
            gen(48, 100),
            gen(17, 50),
            gen(52, 10),
            gen(43, key_range(35, 90)),
            gen(48, 200),
            gen(52, 20),
            gen(43, key_range(40, 80)),
            gen(54, 1),
            gen(58, 62),
            gen(0, 2),
            gen(2, 3),
            gen(53, 0),
            gen(0, 0),
        ]
    )
    ibag = bag(0) + bag(4) + bag(12)

    pdta = b"".join(
        [
            chunk(b"phdr", phdr("Preset", 0, 0, 0) + phdr("EOP", 0, 0, 2)),
            chunk(b"pbag", pbag),
            chunk(b"pmod", b"\0" * 10),
            chunk(b"pgen", pgen),
            chunk(b"inst", inst("Instrument", 0) + inst("EOI", 2)),
            chunk(b"ibag", ibag),
            chunk(b"imod", b"\0" * 10),
            chunk(b"igen", igen),
            chunk(
                b"shdr",
                shdr("Sample", 0, 100, 10, 90)
                + shdr("EOS", 100, 100, 100, 100),
            ),
        ]
    )
    sdta = chunk(b"smpl", b"\0\0" * 160)
    info = chunk(b"ifil", struct.pack("<HH", 2, 4))
    body = b"sfbk" + list_chunk(b"INFO", info) + list_chunk(b"sdta", sdta) + list_chunk(b"pdta", pdta)
    return b"RIFF" + struct.pack("<I", len(body)) + body


class ResolvedZoneTests(unittest.TestCase):
    def parse_fixture(self) -> Sf2:
        tmp = tempfile.NamedTemporaryFile(suffix=".sf2", delete=False)
        tmp.write(hierarchy_sf2())
        tmp.close()
        self.addCleanup(lambda: Path(tmp.name).unlink(missing_ok=True))
        return Sf2(Path(tmp.name)).parse()

    def test_four_level_generator_hierarchy(self):
        sf2 = self.parse_fixture()
        zones = resolve_note(sf2, bank=0, program=0, note=60, velocity=100)
        self.assertEqual(len(zones), 1)
        z = zones[0]

        # Range = intersection of preset-global/local and instrument-global/local.
        self.assertEqual(z["key_range"], [40, 80])
        self.assertEqual(z["vel_range"], [0, 127])

        # Instrument local supersedes instrument global; preset local supersedes
        # preset global, then preset values increment the instrument result.
        self.assertEqual(z["initial_attenuation_cb"], 200)
        self.assertEqual(z["pan_sf2_units"], 50)
        self.assertEqual(z["fine_tune_cents"], 25)  # instrument 20 + preset 5
        self.assertEqual(z["coarse_tune_semitones"], 2)

        self.assertEqual(z["root_key"], 62)
        self.assertEqual(z["sample_modes"], 1)
        self.assertEqual(z["sample_start"], 2)
        self.assertEqual(z["sample_end"], 100)
        self.assertEqual(z["loop_start"], 13)
        self.assertEqual(z["loop_end"], 90)
        self.assertEqual(z["sample_pitch_correction_cents"], -2)

        # sampleModes at preset level is invalid and must not overwrite the
        # instrument-level sample mode.
        self.assertTrue(any("sampleModes" in w for w in z["warnings"]))

    def test_note_outside_resolved_range_does_not_trigger(self):
        sf2 = self.parse_fixture()
        self.assertEqual(resolve_note(sf2, 0, 0, note=20, velocity=100), [])

    def test_unfiltered_resolution_returns_zone(self):
        sf2 = self.parse_fixture()
        zones = resolve_preset_zones(sf2, 0)
        self.assertEqual(len(zones), 1)
        self.assertEqual(zones[0]["sample_name"], "Sample")


if __name__ == "__main__":
    unittest.main()
