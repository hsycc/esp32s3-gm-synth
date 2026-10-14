from __future__ import annotations

from typing import Dict, Iterable, List, Optional, Sequence, Tuple

from .core import GEN_NAMES, Gen, Sf2


# SoundFont generator defaults, kept in native SF2 units.
# None means "no override" rather than a numeric generator value.
GEN_DEFAULTS: Dict[int, Optional[int]] = {
    0: 0, 1: 0, 2: 0, 3: 0, 4: 0,
    5: 0, 6: 0, 7: 0, 8: 13500, 9: 0,
    10: 0, 11: 0, 12: 0, 13: 0,
    15: 0, 16: 0, 17: 0,
    21: -12000, 22: 0, 23: -12000, 24: 0,
    25: -12000, 26: -12000, 27: -12000, 28: -12000,
    29: 0, 30: -12000, 31: 0, 32: 0,
    33: -12000, 34: -12000, 35: -12000, 36: -12000,
    37: 0, 38: -12000, 39: 0, 40: 0,
    45: 0,
    46: None, 47: None, 48: 0, 50: 0, 51: 0, 52: 0,
    54: 0, 56: 100, 57: 0, 58: None,
}

# SF2 generators which are invalid at preset level and therefore must not be
# applied as preset offsets. This mirrors the invalid-preset-generator set
# documented by the SoundFont rules and used by FluidSynth's loader.
PRESET_FORBIDDEN = {
    0, 1, 2, 3, 4, 12, 45, 46, 47, 50, 54, 57, 58,
}

# Structural operators handled separately from numeric generator merging.
STRUCTURAL = {41, 43, 44, 53, 60}
UNSIGNED_VALUE_OPS = {41, 46, 47, 53, 54, 57, 58}


def _s16(value: int) -> int:
    return value - 0x10000 if value & 0x8000 else value


def _value(g: Gen) -> int:
    return g.amount if g.oper in UNSIGNED_VALUE_OPS else _s16(g.amount)


def _last_by_oper(gens: Sequence[Gen]) -> Dict[int, int]:
    out: Dict[int, int] = {}
    for g in gens:
        if g.oper not in STRUCTURAL:
            out[g.oper] = _value(g)
    return out


def _range(gens: Sequence[Gen], oper: int) -> Tuple[int, int]:
    result = (0, 127)
    for g in gens:
        if g.oper == oper:
            lo, hi = g.rng()
            result = (max(0, min(127, lo)), max(0, min(127, hi)))
    return result


def _intersect(*ranges: Tuple[int, int]) -> Optional[Tuple[int, int]]:
    lo = max(r[0] for r in ranges)
    hi = min(r[1] for r in ranges)
    return (lo, hi) if lo <= hi else None


def _index(gens: Sequence[Gen], oper: int) -> Optional[int]:
    value = None
    for g in gens:
        if g.oper == oper:
            value = g.amount
    return value


def _merge_local_over_global(global_gens: Sequence[Gen], local_gens: Sequence[Gen]) -> Dict[int, int]:
    """Instrument/preset local generators supersede identical global generators."""
    out = _last_by_oper(global_gens)
    out.update(_last_by_oper(local_gens))
    return out


def _native_generator_state(inst_values: Dict[int, int], preset_values: Dict[int, int]) -> Dict[int, Optional[int]]:
    """Resolve numeric generator values in native SF2 units.

    Instrument values replace defaults. Preset values are offsets added after
    preset-global/local supersession. Invalid preset-level generators are ignored.
    """
    result: Dict[int, Optional[int]] = dict(GEN_DEFAULTS)
    for oper, value in inst_values.items():
        if oper in STRUCTURAL:
            continue
        result[oper] = value
    for oper, value in preset_values.items():
        if oper in STRUCTURAL or oper in PRESET_FORBIDDEN:
            continue
        base = result.get(oper)
        result[oper] = value if base is None else base + value
    return result


def _named(values: Dict[int, Optional[int]]) -> Dict[str, Optional[int]]:
    return {GEN_NAMES.get(oper, f"oper_{oper}"): value for oper, value in sorted(values.items())}


def resolve_preset_zones(
    sf2: Sf2,
    preset_index: int,
    note: Optional[int] = None,
    velocity: Optional[int] = None,
) -> List[dict]:
    """Flatten one preset into sample-playing zones.

    This stage resolves generator hierarchy only. Modulator/default-modulator
    semantics are intentionally handled by the next milestone.
    """
    if preset_index < 0 or preset_index >= len(sf2.phdr) - 1:
        raise IndexError("preset_index out of range")
    if note is not None and not 0 <= note <= 127:
        raise ValueError("note must be 0..127")
    if velocity is not None and not 0 <= velocity <= 127:
        raise ValueError("velocity must be 0..127")

    preset = sf2.phdr[preset_index]
    preset_global: Sequence[Gen] = ()
    preset_locals: List[Tuple[Sequence[Gen], int]] = []

    for zi in sf2._preset_zones(preset_index):
        gens = sf2._gens(sf2.pbag, sf2.pgen, zi)
        instrument_id = _index(gens, 41)
        if instrument_id is None:
            preset_global = gens
        else:
            preset_locals.append((gens, instrument_id))

    resolved: List[dict] = []
    smpl_points = sf2.smpl_bytes // 2

    for preset_local, instrument_id in preset_locals:
        if instrument_id >= len(sf2.inst) - 1:
            continue

        instrument = sf2.inst[instrument_id]
        preset_key = _intersect(_range(preset_global, 43), _range(preset_local, 43))
        preset_vel = _intersect(_range(preset_global, 44), _range(preset_local, 44))
        if preset_key is None or preset_vel is None:
            continue

        preset_values = _merge_local_over_global(preset_global, preset_local)
        preset_invalid_ops = sorted(op for op in preset_values if op in PRESET_FORBIDDEN)

        inst_global: Sequence[Gen] = ()
        inst_locals: List[Tuple[Sequence[Gen], int]] = []
        for zi in sf2._inst_zones(instrument_id):
            gens = sf2._gens(sf2.ibag, sf2.igen, zi)
            sample_id = _index(gens, 53)
            if sample_id is None:
                inst_global = gens
            else:
                inst_locals.append((gens, sample_id))

        for inst_local, sample_id in inst_locals:
            if sample_id >= len(sf2.shdr) - 1:
                continue

            inst_key = _intersect(_range(inst_global, 43), _range(inst_local, 43))
            inst_vel = _intersect(_range(inst_global, 44), _range(inst_local, 44))
            if inst_key is None or inst_vel is None:
                continue
            key_range = _intersect(preset_key, inst_key)
            vel_range = _intersect(preset_vel, inst_vel)
            if key_range is None or vel_range is None:
                continue
            if note is not None and not key_range[0] <= note <= key_range[1]:
                continue
            if velocity is not None and not vel_range[0] <= velocity <= vel_range[1]:
                continue

            inst_values = _merge_local_over_global(inst_global, inst_local)
            values = _native_generator_state(inst_values, preset_values)
            sample = sf2.shdr[sample_id]

            def gv(op: int, fallback: int = 0) -> int:
                value = values.get(op, fallback)
                return fallback if value is None else int(value)

            start = sample.start + gv(0) + gv(4) * 32768
            end = sample.end + gv(1) + gv(12) * 32768
            loop_start = sample.start_loop + gv(2) + gv(45) * 32768
            loop_end = sample.end_loop + gv(3) + gv(50) * 32768
            root_key = values.get(58)
            if root_key is None:
                root_key = sample.root

            warnings: List[str] = []
            if preset_invalid_ops:
                warnings.append(
                    "ignored invalid preset-level generators: "
                    + ", ".join(GEN_NAMES.get(op, f"oper_{op}") for op in preset_invalid_ops)
                )
            if start < 0 or end > smpl_points or start >= end:
                warnings.append("resolved sample address range is outside smpl bounds")
            sample_mode = gv(54)
            if sample_mode & 0x3 and not (start <= loop_start < loop_end <= end):
                warnings.append("resolved loop range is invalid for a looping zone")

            resolved.append({
                "preset_index": preset_index,
                "preset_name": preset.name,
                "bank": preset.bank,
                "program": preset.program,
                "instrument_id": instrument_id,
                "instrument_name": instrument.name,
                "sample_id": sample_id,
                "sample_name": sample.name,
                "key_range": list(key_range),
                "vel_range": list(vel_range),
                "sample_start": start,
                "sample_end": end,
                "loop_start": loop_start,
                "loop_end": loop_end,
                "sample_rate": sample.rate,
                "sample_root_key": sample.root,
                "sample_pitch_correction_cents": sample.correction,
                "sample_type": sample.type,
                "sample_link": sample.link,
                "root_key": int(root_key),
                "coarse_tune_semitones": gv(51),
                "fine_tune_cents": gv(52),
                "scale_tuning_cents_per_key": gv(56, 100),
                "initial_attenuation_cb": gv(48),
                "pan_sf2_units": gv(17),
                "initial_filter_fc_cents": gv(8, 13500),
                "initial_filter_q_cb": gv(9),
                "sample_modes": sample_mode,
                "exclusive_class": gv(57),
                "generators_native": _named(values),
                "warnings": warnings,
            })

    return resolved


def find_preset_index(sf2: Sf2, bank: int, program: int) -> Optional[int]:
    for index, preset in enumerate(sf2.phdr[:-1]):
        if preset.bank == bank and preset.program == program:
            return index
    return None


def resolve_note(sf2: Sf2, bank: int, program: int, note: int, velocity: int) -> List[dict]:
    preset_index = find_preset_index(sf2, bank, program)
    if preset_index is None:
        return []
    return resolve_preset_zones(sf2, preset_index, note=note, velocity=velocity)
