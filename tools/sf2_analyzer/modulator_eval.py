from __future__ import annotations

import math
from typing import Mapping, Optional, Tuple

from .compat import DEFAULT_RUNTIME_COMPATIBILITY, REALWORLD_SF2, STRICT_SF2
from .modulator import ModSource, Modulator, default_modulators


MIDI_RANGE = 128.0
PITCH_WHEEL_RANGE = 16384.0
PEAK_ATTENUATION_CB = 960.0
_CURVE_TABLE_SIZE = 128
_DEFAULT_VELOCITY_FILTER_IDENTITY = next(
    mod.identity
    for mod in default_modulators()
    if mod.destination == 8 and mod.origin == "default:velocity_to_filter_fc"
)


def _concave_table_value(index: int) -> float:
    if index <= 0:
        return 0.0
    if index >= _CURVE_TABLE_SIZE - 1:
        return 1.0
    # FluidSynth follows the shape shown in SF2.01 rather than the erroneous
    # printed equation. This is the same table-generation equation.
    return (-400.0 / PEAK_ATTENUATION_CB) * math.log10(
        ((_CURVE_TABLE_SIZE - 1) - index) / (_CURVE_TABLE_SIZE - 1.0)
    )


def _convex_table_value(index: int) -> float:
    if index <= 0:
        return 0.0
    if index >= _CURVE_TABLE_SIZE - 1:
        return 1.0
    return 1.0 - (-400.0 / PEAK_ATTENUATION_CB) * math.log10(
        index / (_CURVE_TABLE_SIZE - 1.0)
    )


def _interp_table(value: float, fn) -> float:
    if value <= 0.0:
        return 0.0
    if value >= _CURVE_TABLE_SIZE - 1:
        return fn(_CURVE_TABLE_SIZE - 1)
    lo = int(value)
    frac = value - lo
    a = fn(lo)
    b = fn(lo + 1)
    return a + (b - a) * frac


def map_source_value(source: ModSource, value: float, value_range: float = MIDI_RANGE) -> float:
    """Map a raw controller value using SF2 direction/polarity/curve flags.

    The implementation follows FluidSynth's SF2 mapping model, including the
    127/128 top-end behavior for ordinary MIDI sources.
    """
    if source.raw == 0:
        return 1.0
    if value_range <= 0.0:
        raise ValueError("value_range must be positive")

    value = max(0.0, min(value, value_range - 1.0))
    normalized = value / value_range
    inverted = 1.0 - 1.0 / value_range - normalized
    top = (value_range - 1.0) / value_range

    if not source.bipolar:
        mapped = inverted if source.negative else normalized
        if source.curve == 3:  # switch
            return 1.0 if mapped >= 0.5 else 0.0
        if source.curve == 1:  # concave
            return min(_interp_table(_CURVE_TABLE_SIZE * mapped, _concave_table_value), top)
        if source.curve == 2:  # convex
            return min(_interp_table(_CURVE_TABLE_SIZE * mapped, _convex_table_value), top)
        if source.curve == 0:
            return mapped
        raise ValueError(f"unsupported SF2 source curve type {source.curve}")

    source_value = inverted if source.negative else normalized
    if source_value == top:
        mapped = source_value
    else:
        mapped = -1.0 + 2.0 * source_value

    if source.curve == 3:
        return 1.0 if mapped >= 0.0 else -1.0
    if source.curve == 1:
        if mapped >= 0.0:
            return min(_interp_table(_CURVE_TABLE_SIZE * mapped, _concave_table_value), top)
        return -_interp_table(_CURVE_TABLE_SIZE * -mapped, _concave_table_value)
    if source.curve == 2:
        if mapped >= 0.0:
            return min(_interp_table(_CURVE_TABLE_SIZE * mapped, _convex_table_value), top)
        return -_interp_table(_CURVE_TABLE_SIZE * -mapped, _convex_table_value)
    if source.curve == 0:
        return mapped
    raise ValueError(f"unsupported SF2 source curve type {source.curve}")


def _source_raw_value(source: ModSource, state: Mapping) -> Tuple[float, float]:
    if source.raw == 0:
        return 1.0, MIDI_RANGE
    if source.is_cc:
        cc = state.get("cc", {})
        return float(cc.get(source.index, 0)), MIDI_RANGE

    if source.index == 2:
        return float(state.get("velocity", 0)), MIDI_RANGE
    if source.index == 3:
        return float(state.get("key", 0)), MIDI_RANGE
    if source.index == 10:
        return float(state.get("poly_pressure", 0)), MIDI_RANGE
    if source.index == 13:
        return float(state.get("channel_pressure", 0)), MIDI_RANGE
    if source.index == 14:
        return float(state.get("pitch_wheel", 8192)), PITCH_WHEEL_RANGE
    if source.index == 16:
        return float(state.get("pitch_wheel_sensitivity", 2)), MIDI_RANGE
    raise ValueError(f"unsupported general-controller source {source.index}")


def evaluate_modulator(
    modulator: Modulator,
    state: Mapping,
    *,
    compatibility_profile: str = DEFAULT_RUNTIME_COMPATIBILITY,
) -> float:
    """Evaluate one effective Modulator in native destination-generator units.

    The real-world profile mirrors FluidSynth/legacy-bank compatibility for the
    standard velocity-to-filter default modulator: that exact modulator identity
    contributes zero. A strict-SF2 profile remains available for analysis.

    V1 currently accepts only the standard linear transform (transform=0).
    Source mapping curves are fully handled for the common SF2 types.
    """
    if compatibility_profile not in (STRICT_SF2, REALWORLD_SF2):
        raise ValueError(f"unknown SF2 compatibility profile {compatibility_profile!r}")
    if modulator.transform != 0:
        raise ValueError(f"unsupported SF2 transform operator {modulator.transform}")
    if modulator.amount == 0:
        return 0.0

    # FluidSynth 2.6.x intentionally suppresses the SF2 default
    # velocity->filter cutoff route for compatibility. Identity excludes amount,
    # so an identical instrument/preset replacement remains covered.
    if (
        compatibility_profile == REALWORLD_SF2
        and modulator.identity == _DEFAULT_VELOCITY_FILTER_IDENTITY
    ):
        return 0.0

    raw1, range1 = _source_raw_value(modulator.source, state)
    raw2, range2 = _source_raw_value(modulator.amount_source, state)
    mapped1 = map_source_value(modulator.source, raw1, range1)
    mapped2 = map_source_value(modulator.amount_source, raw2, range2)
    return float(modulator.amount) * mapped1 * mapped2


def evaluate_mod_plan(
    plan: Mapping,
    state: Mapping,
    trigger: Optional[str] = None,
    *,
    compatibility_profile: str = DEFAULT_RUNTIME_COMPATIBILITY,
) -> dict:
    """Evaluate effective modulators, optionally restricted to one event group.

    Returned values are summed by destination generator and remain in native
    SoundFont units. Firmware can update only the destinations affected by the
    incoming NoteOn/CC/Pitch/Pressure event.
    """
    items = plan["modulators"]
    if trigger is None:
        indices = range(len(items))
    else:
        indices = plan.get("groups", {}).get(trigger, [])

    totals = {}
    for index in indices:
        item = items[index]
        mod = Modulator(
            item["source_raw"],
            item["destination"],
            item["amount"],
            item["amount_source_raw"],
            item["transform"],
            item.get("origin", "compiled"),
        )
        value = evaluate_modulator(
            mod,
            state,
            compatibility_profile=compatibility_profile,
        )
        totals[mod.destination] = totals.get(mod.destination, 0.0) + value
    return totals
