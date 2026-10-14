from __future__ import annotations

import argparse
import hashlib
import json
import struct
from dataclasses import dataclass
from pathlib import Path

REC = {
    "phdr": 38,
    "pbag": 4,
    "pmod": 10,
    "pgen": 4,
    "inst": 22,
    "ibag": 4,
    "imod": 10,
    "igen": 4,
    "shdr": 46,
}


def pad_even(data: bytes) -> bytes:
    return data + (b"\0" if len(data) & 1 else b"")


def chunk(chunk_id: bytes, payload: bytes) -> bytes:
    return chunk_id + struct.pack("<I", len(payload)) + pad_even(payload)


def list_chunk(kind: bytes, payload: bytes) -> bytes:
    return chunk(b"LIST", kind + payload)


def cstr(raw: bytes) -> str:
    return raw.split(b"\0", 1)[0].decode("latin-1", errors="replace")


def fixed20(value: str) -> bytes:
    raw = value.encode("latin-1", errors="replace")[:20]
    return raw + b"\0" * (20 - len(raw))


@dataclass
class Phdr:
    name: str
    preset: int
    bank: int
    bag: int
    library: int
    genre: int
    morphology: int


@dataclass
class Bag:
    gen: int
    mod: int


@dataclass
class Gen:
    oper: int
    amount: int
    raw: bytes


@dataclass
class Inst:
    name: str
    bag: int


@dataclass
class Shdr:
    name: str
    start: int
    end: int
    loop_start: int
    loop_end: int
    rate: int
    root: int
    correction: int
    link: int
    sample_type: int

    @property
    def pcm_bytes(self) -> int:
        return max(0, self.end - self.start) * 2


class SoundFont:
    """Small host-only SF2 reader/rebuilder for deterministic test subsets."""

    def __init__(self, path: Path):
        self.path = Path(path)
        self.data = self.path.read_bytes()
        self.index = {}
        self.info_raw = None

    def parse(self) -> "SoundFont":
        data = self.data
        if data[:4] != b"RIFF" or data[8:12] != b"sfbk":
            raise ValueError("not a SoundFont 2 RIFF/sfbk file")

        riff_end = min(len(data), 8 + struct.unpack_from("<I", data, 4)[0])
        offset = 12
        while offset + 8 <= riff_end:
            chunk_id = data[offset : offset + 4]
            size = struct.unpack_from("<I", data, offset + 4)[0]
            stop = offset + 8 + size
            if stop > riff_end:
                raise ValueError("top-level RIFF chunk exceeds file bounds")
            raw_chunk = data[offset : stop + (size & 1)]
            if chunk_id == b"LIST":
                kind = data[offset + 8 : offset + 12]
                if kind == b"INFO":
                    self.info_raw = raw_chunk
                pos = offset + 12
                list_end = offset + 8 + size
                while pos + 8 <= list_end:
                    sub_id = data[pos : pos + 4]
                    sub_size = struct.unpack_from("<I", data, pos + 4)[0]
                    self.index[(kind, sub_id)] = (pos + 8, sub_size)
                    pos += 8 + sub_size + (sub_size & 1)
            offset = stop + (size & 1)

        def raw(kind: bytes, chunk_id: bytes) -> bytes:
            start, size = self.index[(kind, chunk_id)]
            return data[start : start + size]

        self.smpl = raw(b"sdta", b"smpl")
        if (b"sdta", b"sm24") in self.index:
            raise ValueError("sm24/24-bit SF2 subset rebuilding is not implemented yet")

        self.phdr = self._parse_phdr(raw(b"pdta", b"phdr"))
        self.pbag = self._parse_bags(raw(b"pdta", b"pbag"), "pbag")
        self.pmod = self._split(raw(b"pdta", b"pmod"), "pmod")
        self.pgen = self._parse_gens(raw(b"pdta", b"pgen"), "pgen")
        self.inst = self._parse_inst(raw(b"pdta", b"inst"))
        self.ibag = self._parse_bags(raw(b"pdta", b"ibag"), "ibag")
        self.imod = self._split(raw(b"pdta", b"imod"), "imod")
        self.igen = self._parse_gens(raw(b"pdta", b"igen"), "igen")
        self.shdr = self._parse_shdr(raw(b"pdta", b"shdr"))
        return self

    @staticmethod
    def _split(data: bytes, name: str):
        size = REC[name]
        if len(data) % size:
            raise ValueError(f"{name} has invalid byte length {len(data)}")
        return [data[i : i + size] for i in range(0, len(data), size)]

    def _parse_phdr(self, data: bytes):
        return [
            Phdr(cstr(record[:20]), *struct.unpack_from("<HHHIII", record, 20))
            for record in self._split(data, "phdr")
        ]

    def _parse_bags(self, data: bytes, name: str):
        return [Bag(*struct.unpack("<HH", record)) for record in self._split(data, name)]

    def _parse_gens(self, data: bytes, name: str):
        return [
            Gen(*struct.unpack("<HH", record), record)
            for record in self._split(data, name)
        ]

    def _parse_inst(self, data: bytes):
        return [
            Inst(cstr(record[:20]), struct.unpack_from("<H", record, 20)[0])
            for record in self._split(data, "inst")
        ]

    def _parse_shdr(self, data: bytes):
        return [
            Shdr(cstr(record[:20]), *struct.unpack_from("<IIIIIBbHH", record, 20))
            for record in self._split(data, "shdr")
        ]

    @staticmethod
    def _gens_for(bags, gens, zone_index):
        return gens[bags[zone_index].gen : bags[zone_index + 1].gen]

    @staticmethod
    def _mods_for(bags, mods, zone_index):
        return mods[bags[zone_index].mod : bags[zone_index + 1].mod]

    def find_preset(self, bank: int, program: int):
        return next(
            (
                i
                for i, preset in enumerate(self.phdr[:-1])
                if preset.bank == bank and preset.preset == program
            ),
            None,
        )

    def dependencies(self, preset_indices):
        instrument_ids = set()
        for preset_index in preset_indices:
            preset = self.phdr[preset_index]
            for zone_index in range(preset.bag, self.phdr[preset_index + 1].bag):
                for generator in self._gens_for(self.pbag, self.pgen, zone_index):
                    if generator.oper == 41:  # instrument
                        instrument_ids.add(generator.amount)

        sample_ids = set()
        for instrument_id in list(instrument_ids):
            if instrument_id >= len(self.inst) - 1:
                continue
            instrument = self.inst[instrument_id]
            for zone_index in range(instrument.bag, self.inst[instrument_id + 1].bag):
                for generator in self._gens_for(self.ibag, self.igen, zone_index):
                    if generator.oper == 53:  # sampleID
                        sample_ids.add(generator.amount)

        # Pull linked/stereo partners into the subset too.
        changed = True
        while changed:
            changed = False
            for sample_id in list(sample_ids):
                if sample_id >= len(self.shdr) - 1:
                    continue
                sample = self.shdr[sample_id]
                base_type = sample.sample_type & 0x7FFF
                if (
                    base_type in (2, 4, 8)
                    and 0 <= sample.link < len(self.shdr) - 1
                    and sample.link not in sample_ids
                ):
                    sample_ids.add(sample.link)
                    changed = True
        return instrument_ids, sample_ids

    def estimated_bytes(self, preset_indices) -> int:
        _, sample_ids = self.dependencies(preset_indices)
        # 46 guard sample points per copied sample plus conservative metadata room.
        return (
            sum(self.shdr[sid].pcm_bytes + 92 for sid in sample_ids)
            + 256 * 1024
        )

    def build_subset(self, preset_indices, output_path: Path):
        preset_indices = sorted(
            set(preset_indices),
            key=lambda i: (self.phdr[i].bank, self.phdr[i].preset, i),
        )
        instrument_ids, sample_ids = self.dependencies(preset_indices)
        instrument_order = sorted(i for i in instrument_ids if i < len(self.inst) - 1)
        sample_order = sorted(i for i in sample_ids if i < len(self.shdr) - 1)
        instrument_map = {old: new for new, old in enumerate(instrument_order)}
        sample_map = {old: new for new, old in enumerate(sample_order)}

        # Rebuild PCM as independent sample regions. Each region receives the
        # SoundFont-required interpolation guard area after its logical end.
        new_smpl = bytearray()
        new_samples = []
        sample_point = 0
        for old_sample_id in sample_order:
            sample = self.shdr[old_sample_id]
            pcm = self.smpl[sample.start * 2 : sample.end * 2]
            sample_points = max(0, sample.end - sample.start)
            new_start = sample_point
            new_end = new_start + sample_points
            loop_start = new_start + max(0, sample.loop_start - sample.start)
            loop_end = new_start + max(0, sample.loop_end - sample.start)
            sample_link = sample_map.get(sample.link, 0)
            new_samples.append(
                Shdr(
                    sample.name,
                    new_start,
                    new_end,
                    loop_start,
                    loop_end,
                    sample.rate,
                    sample.root,
                    sample.correction,
                    sample_link,
                    sample.sample_type,
                )
            )
            new_smpl += pcm
            new_smpl += b"\0" * (46 * 2)
            sample_point = new_end + 46

        # Rebuild instruments and remap sampleID generators.
        new_inst = []
        new_ibag = []
        new_imod = []
        new_igen = []
        for old_instrument_id in instrument_order:
            instrument = self.inst[old_instrument_id]
            new_inst.append(Inst(instrument.name, len(new_ibag)))
            for zone_index in range(
                instrument.bag, self.inst[old_instrument_id + 1].bag
            ):
                new_ibag.append(Bag(len(new_igen), len(new_imod)))
                new_imod.extend(self._mods_for(self.ibag, self.imod, zone_index))
                for generator in self._gens_for(self.ibag, self.igen, zone_index):
                    amount = generator.amount
                    if generator.oper == 53:
                        if amount not in sample_map:
                            continue
                        amount = sample_map[amount]
                    new_igen.append(
                        Gen(generator.oper, amount, struct.pack("<HH", generator.oper, amount))
                    )
        new_ibag.append(Bag(len(new_igen), len(new_imod)))
        new_igen.append(Gen(0, 0, b"\0" * 4))
        new_imod.append(b"\0" * 10)

        # Rebuild presets and remap instrument generators.
        new_phdr = []
        new_pbag = []
        new_pmod = []
        new_pgen = []
        for old_preset_index in preset_indices:
            preset = self.phdr[old_preset_index]
            new_phdr.append(
                Phdr(
                    preset.name,
                    preset.preset,
                    preset.bank,
                    len(new_pbag),
                    preset.library,
                    preset.genre,
                    preset.morphology,
                )
            )
            for zone_index in range(
                preset.bag, self.phdr[old_preset_index + 1].bag
            ):
                new_pbag.append(Bag(len(new_pgen), len(new_pmod)))
                new_pmod.extend(self._mods_for(self.pbag, self.pmod, zone_index))
                for generator in self._gens_for(self.pbag, self.pgen, zone_index):
                    amount = generator.amount
                    if generator.oper == 41:
                        if amount not in instrument_map:
                            continue
                        amount = instrument_map[amount]
                    new_pgen.append(
                        Gen(generator.oper, amount, struct.pack("<HH", generator.oper, amount))
                    )
        new_pbag.append(Bag(len(new_pgen), len(new_pmod)))
        new_pgen.append(Gen(0, 0, b"\0" * 4))
        new_pmod.append(b"\0" * 10)

        eop = Phdr("EOP", 0, 0, len(new_pbag) - 1, 0, 0, 0)
        eoi = Inst("EOI", len(new_ibag) - 1)
        eos = Shdr(
            "EOS",
            sample_point,
            sample_point,
            sample_point,
            sample_point,
            44100,
            0,
            0,
            0,
            1,
        )

        def phdr_bytes(items):
            return b"".join(
                fixed20(p.name)
                + struct.pack(
                    "<HHHIII",
                    p.preset,
                    p.bank,
                    p.bag,
                    p.library,
                    p.genre,
                    p.morphology,
                )
                for p in items
            )

        def bag_bytes(items):
            return b"".join(struct.pack("<HH", b.gen, b.mod) for b in items)

        def inst_bytes(items):
            return b"".join(fixed20(i.name) + struct.pack("<H", i.bag) for i in items)

        def shdr_bytes(items):
            return b"".join(
                fixed20(s.name)
                + struct.pack(
                    "<IIIIIBbHH",
                    s.start,
                    s.end,
                    s.loop_start,
                    s.loop_end,
                    s.rate,
                    s.root,
                    s.correction,
                    s.link,
                    s.sample_type,
                )
                for s in items
            )

        pdta = b"".join(
            [
                chunk(b"phdr", phdr_bytes(new_phdr + [eop])),
                chunk(b"pbag", bag_bytes(new_pbag)),
                chunk(b"pmod", b"".join(new_pmod)),
                chunk(b"pgen", b"".join(g.raw for g in new_pgen)),
                chunk(b"inst", inst_bytes(new_inst + [eoi])),
                chunk(b"ibag", bag_bytes(new_ibag)),
                chunk(b"imod", b"".join(new_imod)),
                chunk(b"igen", b"".join(g.raw for g in new_igen)),
                chunk(b"shdr", shdr_bytes(new_samples + [eos])),
            ]
        )
        info = self.info_raw or list_chunk(
            b"INFO", chunk(b"ifil", struct.pack("<HH", 2, 4))
        )
        body = (
            b"sfbk"
            + info
            + list_chunk(b"sdta", chunk(b"smpl", bytes(new_smpl)))
            + list_chunk(b"pdta", pdta)
        )
        output = b"RIFF" + struct.pack("<I", len(body)) + body
        output_path.parent.mkdir(parents=True, exist_ok=True)
        output_path.write_bytes(output)

        return {
            "output": str(output_path),
            "size_bytes": len(output),
            "sha256": hashlib.sha256(output).hexdigest(),
            "source": str(self.path),
            "source_sha256": hashlib.sha256(self.data).hexdigest(),
            "preset_count": len(preset_indices),
            "instrument_count": len(instrument_order),
            "sample_count": len(sample_order),
            "presets": [
                {
                    "bank": self.phdr[i].bank,
                    "program": self.phdr[i].preset,
                    "name": self.phdr[i].name,
                }
                for i in preset_indices
            ],
        }


def choose_diverse_gm_subset(sf2: SoundFont, target_bytes: int):
    """Choose a diverse GM subset while staying close to a byte budget.

    The order walks one program from every 8-program GM family before taking
    the next member of each family: 0, 8, 16, ... 120, then 1, 9, 17, ...
    A standard bank-128/program-0 drum kit is included first when available.
    """
    selected = []
    drum = sf2.find_preset(128, 0)
    if drum is not None:
        selected.append(drum)

    candidates = []
    for offset in range(8):
        for family in range(16):
            program = family * 8 + offset
            preset_index = sf2.find_preset(0, program)
            if preset_index is not None:
                candidates.append(preset_index)

    # Leave room for PDTA/INFO tables. Exact final size is written to manifest.
    budget = max(256 * 1024, target_bytes - 128 * 1024)
    for preset_index in candidates:
        if preset_index in selected:
            continue
        trial = selected + [preset_index]
        if sf2.estimated_bytes(trial) <= budget:
            selected.append(preset_index)
    return selected


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Build a deterministic, valid SF2 subset for compatibility tests"
    )
    parser.add_argument("sf2", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--target-mib", type=float, default=6.0)
    parser.add_argument("--manifest", type=Path)
    args = parser.parse_args(argv)

    sf2 = SoundFont(args.sf2).parse()
    selected = choose_diverse_gm_subset(sf2, int(args.target_mib * 1024 * 1024))
    manifest = sf2.build_subset(selected, args.output)
    manifest["target_mib"] = args.target_mib
    manifest["selection_method"] = "gm_family_round_robin_with_standard_drum"

    text = json.dumps(manifest, indent=2, ensure_ascii=False)
    if args.manifest:
        args.manifest.parent.mkdir(parents=True, exist_ok=True)
        args.manifest.write_text(text + "\n", encoding="utf-8")
    print(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
