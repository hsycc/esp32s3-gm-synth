from __future__ import annotations

import argparse
import json
import subprocess
from pathlib import Path
from typing import List, Mapping, Tuple

from tools.sf2_analyzer.compat import runtime_generator_base
from tools.sf2_analyzer.core import GEN_NAMES, Sf2
from tools.sf2_analyzer.modulator import compile_note_mod_plans
from tools.sf2_analyzer.modulator_eval import evaluate_mod_plan
from tools.sf2_analyzer.resolved_zone import find_preset_index, resolve_note


GENERATOR_OPS = (6, 8, 9, 15, 16, 17, 48, 51, 52, 56)
GENERATOR_NAMES = {op: GEN_NAMES[op] for op in GENERATOR_OPS}

# Normalization is used only to pair equivalent voices when a preset creates
# more than one voice. It is not an acceptance tolerance.
PAIR_SCALES = {
    6: 100.0,
    8: 1200.0,
    9: 100.0,
    15: 1000.0,
    16: 1000.0,
    17: 500.0,
    48: 960.0,
    51: 12.0,
    52: 100.0,
    56: 100.0,
}

DEFAULT_STATE = {
    "cc": {1: 0, 7: 100, 10: 64, 11: 127, 91: 0, 93: 0},
    "channel_pressure": 0,
    "poly_pressure": 0,
    "pitch_wheel": 8192,
    "pitch_wheel_sensitivity": 2,
}


def _parse_probe_output(text: str) -> dict:
    meta = None
    voices = []
    for raw_line in text.splitlines():
        if not raw_line.strip():
            continue
        fields = raw_line.split("\t")
        if fields[0] == "META":
            if len(fields) != 6:
                raise ValueError(f"bad META row: {raw_line}")
            meta = {
                "voice_count": int(fields[1]),
                "bank": int(fields[2]),
                "program": int(fields[3]),
                "key": int(fields[4]),
                "velocity": int(fields[5]),
            }
        elif fields[0] == "VOICE":
            if len(fields) != 4 + len(GENERATOR_OPS):
                raise ValueError(f"bad VOICE row: {raw_line}")
            values = {
                op: float(fields[4 + i]) for i, op in enumerate(GENERATOR_OPS)
            }
            voices.append(
                {
                    "oracle_index": int(fields[1]),
                    "actual_key": int(fields[2]),
                    "actual_velocity": int(fields[3]),
                    "generators": values,
                }
            )
        else:
            raise ValueError(f"unknown probe row: {raw_line}")
    if meta is None:
        raise ValueError("FluidSynth probe produced no META row")
    if meta["voice_count"] != len(voices):
        raise ValueError("FluidSynth probe voice count disagrees with VOICE rows")
    return {"meta": meta, "voices": voices}


def _run_probe(probe: Path, sf2: Path, bank: int, program: int, key: int, velocity: int) -> dict:
    result = subprocess.run(
        [str(probe), str(sf2), str(bank), str(program), str(key), str(velocity)],
        check=False,
        text=True,
        capture_output=True,
    )
    if result.returncode != 0:
        raise RuntimeError(
            f"FluidSynth probe failed ({result.returncode})\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return _parse_probe_output(result.stdout)


def _effective_key_velocity(zone: Mapping, key: int, velocity: int) -> Tuple[int, int]:
    native = zone["generators_native"]
    fixed_key = native.get("keynum")
    fixed_velocity = native.get("velocity")
    return (
        key if fixed_key is None else int(fixed_key),
        velocity if fixed_velocity is None else int(fixed_velocity),
    )


def _our_voices(sf2_path: Path, bank: int, program: int, key: int, velocity: int) -> List[dict]:
    sf2 = Sf2(sf2_path).parse()
    preset_index = find_preset_index(sf2, bank, program)
    if preset_index is None:
        raise ValueError(f"preset not found: bank={bank} program={program}")

    zones = resolve_note(sf2, bank, program, key, velocity)
    plans = compile_note_mod_plans(sf2, preset_index, key, velocity)
    if len(zones) != len(plans):
        raise AssertionError(
            f"resolver/modulator zone count mismatch: zones={len(zones)} plans={len(plans)}"
        )

    state = dict(DEFAULT_STATE)
    state["key"] = key
    state["velocity"] = velocity

    voices = []
    for zone, compiled in zip(zones, plans):
        if zone["sample_id"] != compiled["sample_id"]:
            raise AssertionError(
                f"resolver/modulator sample mismatch: {zone['sample_id']} != {compiled['sample_id']}"
            )
        modulation = evaluate_mod_plan(compiled["plan"], state)
        native = zone["generators_native"]
        values = {}
        for op in GENERATOR_OPS:
            name = GENERATOR_NAMES[op]
            native_base = native.get(name)
            if native_base is None:
                native_base = 0
            runtime_base = runtime_generator_base(op, native_base)
            values[op] = float(runtime_base or 0.0) + float(modulation.get(op, 0.0))
        actual_key, actual_velocity = _effective_key_velocity(zone, key, velocity)
        voices.append(
            {
                "sample_id": zone["sample_id"],
                "sample_name": zone["sample_name"],
                "instrument_id": zone["instrument_id"],
                "actual_key": actual_key,
                "actual_velocity": actual_velocity,
                "generators": values,
            }
        )
    return voices


def _pair_cost(ours: Mapping, oracle: Mapping) -> float:
    if ours["actual_key"] != oracle["actual_key"]:
        return 1_000_000.0 + abs(ours["actual_key"] - oracle["actual_key"])
    if ours["actual_velocity"] != oracle["actual_velocity"]:
        return 100_000.0 + abs(ours["actual_velocity"] - oracle["actual_velocity"])
    return sum(
        abs(ours["generators"][op] - oracle["generators"][op]) / PAIR_SCALES[op]
        for op in GENERATOR_OPS
    )


def _pair_voices(ours: List[dict], oracle: List[dict]) -> List[Tuple[int, int, float]]:
    candidates = []
    for oi, our_voice in enumerate(ours):
        for fi, fluid_voice in enumerate(oracle):
            candidates.append((_pair_cost(our_voice, fluid_voice), oi, fi))
    candidates.sort()

    used_ours = set()
    used_oracle = set()
    pairs = []
    for cost, oi, fi in candidates:
        if oi in used_ours or fi in used_oracle:
            continue
        used_ours.add(oi)
        used_oracle.add(fi)
        pairs.append((oi, fi, cost))
    return pairs


def compare_case(
    probe: Path,
    sf2: Path,
    bank: int,
    program: int,
    key: int,
    velocity: int,
    tolerance: float = 1e-3,
) -> dict:
    oracle = _run_probe(probe, sf2, bank, program, key, velocity)
    ours = _our_voices(sf2, bank, program, key, velocity)
    pairs = _pair_voices(ours, oracle["voices"])

    pair_reports = []
    generator_mismatch_count = 0
    key_velocity_mismatch_count = 0
    max_abs_delta = 0.0
    for oi, fi, cost in pairs:
        our_voice = ours[oi]
        fluid_voice = oracle["voices"][fi]
        actual_match = (
            our_voice["actual_key"] == fluid_voice["actual_key"]
            and our_voice["actual_velocity"] == fluid_voice["actual_velocity"]
        )
        if not actual_match:
            key_velocity_mismatch_count += 1

        generators = {}
        for op in GENERATOR_OPS:
            delta = our_voice["generators"][op] - fluid_voice["generators"][op]
            max_abs_delta = max(max_abs_delta, abs(delta))
            ok = abs(delta) <= tolerance
            if not ok:
                generator_mismatch_count += 1
            generators[GENERATOR_NAMES[op]] = {
                "op": op,
                "ours": our_voice["generators"][op],
                "fluidsynth": fluid_voice["generators"][op],
                "delta": delta,
                "match": ok,
            }

        pair_reports.append(
            {
                "our_index": oi,
                "oracle_index": fi,
                "pair_cost": cost,
                "sample_id": our_voice["sample_id"],
                "sample_name": our_voice["sample_name"],
                "actual_key": {
                    "ours": our_voice["actual_key"],
                    "fluidsynth": fluid_voice["actual_key"],
                },
                "actual_velocity": {
                    "ours": our_voice["actual_velocity"],
                    "fluidsynth": fluid_voice["actual_velocity"],
                },
                "actual_key_velocity_match": actual_match,
                "generators": generators,
            }
        )

    voice_count_match = len(ours) == len(oracle["voices"])
    structural_ok = voice_count_match and key_velocity_mismatch_count == 0
    generators_ok = structural_ok and generator_mismatch_count == 0
    return {
        "schema_version": 1,
        "sf2": str(sf2),
        "case": {
            "bank": bank,
            "program": program,
            "key": key,
            "velocity": velocity,
            "controller_state": DEFAULT_STATE,
        },
        "voice_count": {
            "ours": len(ours),
            "fluidsynth": len(oracle["voices"]),
            "match": voice_count_match,
        },
        "paired_voice_count": len(pairs),
        "key_velocity_mismatch_count": key_velocity_mismatch_count,
        "generator_mismatch_count": generator_mismatch_count,
        "max_abs_generator_delta": max_abs_delta,
        "structural_ok": structural_ok,
        "generators_ok": generators_ok,
        "pairs": pair_reports,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description="Compare one SF2 note against FluidSynth voice generators")
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--sf2", type=Path, required=True)
    parser.add_argument("--bank", type=int, default=0)
    parser.add_argument("--program", type=int, required=True)
    parser.add_argument("--key", type=int, default=60)
    parser.add_argument("--velocity", type=int, default=96)
    parser.add_argument("--tolerance", type=float, default=1e-3)
    parser.add_argument("--json", type=Path)
    parser.add_argument("--strict-generators", action="store_true")
    args = parser.parse_args()

    report = compare_case(
        args.probe,
        args.sf2,
        args.bank,
        args.program,
        args.key,
        args.velocity,
        args.tolerance,
    )
    text = json.dumps(report, indent=2, sort_keys=True)
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(text + "\n", encoding="utf-8")

    print(
        f"{args.sf2.name} bank={args.bank} program={args.program} key={args.key} vel={args.velocity}: "
        f"voices {report['voice_count']['ours']}/{report['voice_count']['fluidsynth']}, "
        f"keyvel_mismatch={report['key_velocity_mismatch_count']}, "
        f"generator_mismatch={report['generator_mismatch_count']}, "
        f"max_delta={report['max_abs_generator_delta']:.6g}"
    )

    if not report["structural_ok"]:
        return 1
    if args.strict_generators and not report["generators_ok"]:
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
