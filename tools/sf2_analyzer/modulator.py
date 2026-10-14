from __future__ import annotations

from dataclasses import dataclass, replace
from typing import Dict, Iterable, List, Optional, Sequence, Tuple

from .core import GEN_NAMES, Sf2
from .resolved_zone import _index, _intersect, _range


CURVE_NAMES = {0: "linear", 1: "concave", 2: "convex", 3: "switch"}
GC_NAMES = {
    0: "none",
    2: "note_on_velocity",
    3: "note_on_key",
    10: "poly_pressure",
    13: "channel_pressure",
    14: "pitch_wheel",
    16: "pitch_wheel_sensitivity",
}


def encode_source(
    index: int,
    *,
    cc: bool = False,
    negative: bool = False,
    bipolar: bool = False,
    curve: int = 0,
) -> int:
    """Encode the SF2 sfModulator/sfModAmountSrc bit field."""
    return (
        (index & 0x7F)
        | (0x80 if cc else 0)
        | (0x100 if negative else 0)
        | (0x200 if bipolar else 0)
        | ((curve & 0x3F) << 10)
    )


@dataclass(frozen=True)
class ModSource:
    raw: int
    index: int
    is_cc: bool
    negative: bool
    bipolar: bool
    curve: int

    @property
    def curve_name(self) -> str:
        return CURVE_NAMES.get(self.curve, f"curve_{self.curve}")

    @property
    def name(self) -> str:
        if self.raw == 0:
            return "none"
        if self.is_cc:
            return f"cc_{self.index}"
        return GC_NAMES.get(self.index, f"general_controller_{self.index}")

    @property
    def trigger_group(self) -> Optional[str]:
        if self.raw == 0:
            return None
        if self.is_cc:
            return f"cc:{self.index}"
        if self.index in (2, 3):
            return "note_on"
        if self.index == 10:
            return "poly_pressure"
        if self.index == 13:
            return "channel_pressure"
        if self.index in (14, 16):
            return "pitch"
        return f"general:{self.index}"


def decode_source(raw: int) -> ModSource:
    return ModSource(
        raw=raw,
        index=raw & 0x7F,
        is_cc=bool(raw & 0x80),
        negative=bool(raw & 0x100),
        bipolar=bool(raw & 0x200),
        curve=(raw >> 10) & 0x3F,
    )


@dataclass(frozen=True)
class Modulator:
    source_raw: int
    destination: int
    amount: int
    amount_source_raw: int = 0
    transform: int = 0
    origin: str = "sf2"

    @property
    def identity(self) -> Tuple[int, int, int, int]:
        # SF2 identity excludes amount. An instrument modulator with amount=0
        # therefore still has the same identity and can disable a default mod.
        return (
            self.source_raw,
            self.destination,
            self.amount_source_raw,
            self.transform,
        )

    @property
    def source(self) -> ModSource:
        return decode_source(self.source_raw)

    @property
    def amount_source(self) -> ModSource:
        return decode_source(self.amount_source_raw)

    @property
    def destination_name(self) -> str:
        return GEN_NAMES.get(self.destination, f"oper_{self.destination}")

    @property
    def disabled(self) -> bool:
        return self.amount == 0

    def as_dict(self) -> dict:
        return {
            "source_raw": self.source_raw,
            "source": self.source.name,
            "source_curve": self.source.curve_name,
            "source_negative": self.source.negative,
            "source_bipolar": self.source.bipolar,
            "destination": self.destination,
            "destination_name": self.destination_name,
            "amount": self.amount,
            "amount_source_raw": self.amount_source_raw,
            "amount_source": self.amount_source.name,
            "amount_source_curve": self.amount_source.curve_name,
            "transform": self.transform,
            "origin": self.origin,
            "disabled": self.disabled,
        }


def _m(src: int, dest: int, amount: int, src2: int = 0, *, name: str) -> Modulator:
    return Modulator(src, dest, amount, src2, 0, f"default:{name}")


def default_modulators() -> List[Modulator]:
    """SoundFont default modulators, aligned with FluidSynth's SF2 defaults.

    The velocity->filter secondary-source polarity follows FluidSynth's
    compatibility choice for real-world SoundFonts (positive switch source).
    """
    return [
        _m(
            encode_source(2, negative=True, curve=1),
            48,
            960,
            name="velocity_to_attenuation",
        ),
        _m(
            encode_source(2, negative=True),
            8,
            -2400,
            encode_source(2, curve=3),
            name="velocity_to_filter_fc",
        ),
        _m(
            encode_source(13),
            6,
            50,
            name="channel_pressure_to_vibrato",
        ),
        _m(
            encode_source(1, cc=True),
            6,
            50,
            name="mod_wheel_to_vibrato",
        ),
        _m(
            encode_source(7, cc=True, negative=True, curve=1),
            48,
            960,
            name="volume_to_attenuation",
        ),
        _m(
            encode_source(10, cc=True, bipolar=True),
            17,
            500,
            name="pan",
        ),
        _m(
            encode_source(11, cc=True, negative=True, curve=1),
            48,
            960,
            name="expression_to_attenuation",
        ),
        _m(
            encode_source(91, cc=True),
            16,
            200,
            name="reverb_send",
        ),
        _m(
            encode_source(93, cc=True),
            15,
            200,
            name="chorus_send",
        ),
        _m(
            encode_source(14, bipolar=True),
            52,
            12700,
            encode_source(16),
            name="pitch_wheel_to_pitch",
        ),
    ]


def modulator_from_tuple(record, origin: str) -> Modulator:
    src, dest, amount, amount_src, transform = record
    return Modulator(src, dest, amount, amount_src, transform, origin)


def _zone_mods(bags, records, zone_index: Optional[int], origin: str) -> List[Modulator]:
    if zone_index is None or zone_index + 1 >= len(bags):
        return []
    start, end = bags[zone_index].mod, bags[zone_index + 1].mod
    if start > end or end > len(records):
        return []
    return [
        modulator_from_tuple(record, origin)
        for record in records[start:end]
        if any(record)
    ]


def _local_supersedes_global(
    global_mods: Sequence[Modulator], local_mods: Sequence[Modulator]
) -> List[Modulator]:
    """Return global+local zone modulators with local identity taking precedence."""
    merged: Dict[Tuple[int, int, int, int], Modulator] = {
        mod.identity: mod for mod in global_mods
    }
    for mod in local_mods:
        merged[mod.identity] = mod
    return list(merged.values())


def merge_modulators(
    instrument_global: Sequence[Modulator] = (),
    instrument_local: Sequence[Modulator] = (),
    preset_global: Sequence[Modulator] = (),
    preset_local: Sequence[Modulator] = (),
    defaults: Optional[Sequence[Modulator]] = None,
) -> List[Modulator]:
    """Resolve SF2 default/instrument/preset modulator semantics.

    Rules mirrored from the SoundFont model and FluidSynth:
    - local zone replaces an identical modulator from its global zone;
    - instrument modulators OVERWRITE identical defaults, including amount=0;
    - preset modulators ADD their amount to an existing identical modulator;
    - zero-amount preset modulators do not cancel instrument/default behavior.
    """
    effective: Dict[Tuple[int, int, int, int], Modulator] = {
        mod.identity: mod for mod in (defaults if defaults is not None else default_modulators())
    }

    instrument = _local_supersedes_global(instrument_global, instrument_local)
    for mod in instrument:
        effective[mod.identity] = mod

    preset = _local_supersedes_global(preset_global, preset_local)
    for mod in preset:
        if mod.amount == 0:
            continue
        previous = effective.get(mod.identity)
        if previous is None:
            effective[mod.identity] = mod
        else:
            effective[mod.identity] = replace(
                previous,
                amount=previous.amount + mod.amount,
                origin=f"{previous.origin}+{mod.origin}",
            )

    return list(effective.values())


def build_mod_plan(modulators: Sequence[Modulator]) -> dict:
    """Group effective modulators by the events which can change their value.

    This is deliberately a control/event-rate plan. It is not a per-sample
    interpreter. Continuous DSP cost is represented by destination flags later
    in the voice engine, not by rescanning this list every audio sample.
    """
    groups: Dict[str, List[int]] = {}
    items = []
    for index, mod in enumerate(modulators):
        item = mod.as_dict()
        triggers = {
            trigger
            for trigger in (
                mod.source.trigger_group,
                mod.amount_source.trigger_group,
            )
            if trigger is not None
        }
        if not triggers:
            triggers = {"constant"}
        item["trigger_groups"] = sorted(triggers)
        items.append(item)
        for trigger in triggers:
            groups.setdefault(trigger, []).append(index)

    return {
        "modulator_count": len(items),
        "enabled_modulator_count": sum(1 for m in modulators if not m.disabled),
        "disabled_modulator_count": sum(1 for m in modulators if m.disabled),
        "groups": {name: indices for name, indices in sorted(groups.items())},
        "modulators": items,
    }


def _matching_zone_indices(sf2: Sf2, preset_index: int, note: int, velocity: int):
    preset_global = None
    preset_locals = []
    for pzi in sf2._preset_zones(preset_index):
        gens = sf2._gens(sf2.pbag, sf2.pgen, pzi)
        instrument_id = _index(gens, 41)
        if instrument_id is None:
            preset_global = pzi
        else:
            preset_locals.append((pzi, gens, instrument_id))

    pg_gens = sf2._gens(sf2.pbag, sf2.pgen, preset_global) if preset_global is not None else ()

    for pzi, pgens, instrument_id in preset_locals:
        if instrument_id >= len(sf2.inst) - 1:
            continue
        pkey = _intersect(_range(pg_gens, 43), _range(pgens, 43))
        pvel = _intersect(_range(pg_gens, 44), _range(pgens, 44))
        if pkey is None or pvel is None or not (pkey[0] <= note <= pkey[1] and pvel[0] <= velocity <= pvel[1]):
            continue

        inst_global = None
        inst_locals = []
        for izi in sf2._inst_zones(instrument_id):
            gens = sf2._gens(sf2.ibag, sf2.igen, izi)
            sample_id = _index(gens, 53)
            if sample_id is None:
                inst_global = izi
            else:
                inst_locals.append((izi, gens, sample_id))

        ig_gens = sf2._gens(sf2.ibag, sf2.igen, inst_global) if inst_global is not None else ()
        for izi, igens, sample_id in inst_locals:
            ikey = _intersect(_range(ig_gens, 43), _range(igens, 43))
            ivel = _intersect(_range(ig_gens, 44), _range(igens, 44))
            if ikey is None or ivel is None:
                continue
            key = _intersect(pkey, ikey)
            vel = _intersect(pvel, ivel)
            if key is None or vel is None:
                continue
            if key[0] <= note <= key[1] and vel[0] <= velocity <= vel[1]:
                yield preset_global, pzi, inst_global, izi, instrument_id, sample_id


def compile_note_mod_plans(
    sf2: Sf2,
    preset_index: int,
    note: int,
    velocity: int,
) -> List[dict]:
    """Compile one ModPlan per sample-playing zone triggered by a note."""
    out = []
    for pglobal, plocal, iglobal, ilocal, instrument_id, sample_id in _matching_zone_indices(
        sf2, preset_index, note, velocity
    ):
        instrument_global = _zone_mods(sf2.ibag, sf2.imod, iglobal, "instrument_global")
        instrument_local = _zone_mods(sf2.ibag, sf2.imod, ilocal, "instrument_local")
        preset_global = _zone_mods(sf2.pbag, sf2.pmod, pglobal, "preset_global")
        preset_local = _zone_mods(sf2.pbag, sf2.pmod, plocal, "preset_local")
        effective = merge_modulators(
            instrument_global,
            instrument_local,
            preset_global,
            preset_local,
        )
        out.append(
            {
                "preset_index": preset_index,
                "preset_zone_index": plocal,
                "instrument_id": instrument_id,
                "instrument_zone_index": ilocal,
                "sample_id": sample_id,
                "plan": build_mod_plan(effective),
            }
        )
    return out
