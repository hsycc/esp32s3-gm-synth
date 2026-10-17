from __future__ import annotations

import argparse
import json
from pathlib import Path

from tools.sf2_analyzer import analyze


DEFAULT_ARENA_BYTES = 4 * 1024 * 1024


def build_pressure_report(sf2_path: Path, arena_bytes: int) -> dict:
    analysis = analyze(sf2_path)
    gm = analysis["gm"]
    working_sets = analysis["working_sets"]
    exact_pair = working_sets["gm_exact_worst_active_ready_union"]

    if gm["bank0_program_count"] != 128:
        raise ValueError(
            f"expected 128 GM bank-0 programs, found {gm['bank0_program_count']}"
        )
    standard_drums = [
        preset for preset in gm["drum_presets"] if preset["program"] == 0
    ]
    if not standard_drums:
        raise ValueError("missing bank-128/program-0 standard drum preset")
    if exact_pair is None:
        raise ValueError("unable to derive exact active+READY program pair")

    required = int(exact_pair["aligned_pcm_bytes"])
    return {
        "schema_version": 1,
        "source": analysis["file"],
        "arena_bytes": arena_bytes,
        "gm_identity_count": 129,
        "bank0_program_count": gm["bank0_program_count"],
        "standard_drum_program": standard_drums[0],
        "exact_worst_active_ready_union": exact_pair,
        "exact_pair_fits_arena": required <= arena_bytes,
        "exact_pair_margin_bytes": arena_bytes - required,
        "all_samples_aligned_bytes": analysis["samples"]["aligned_pcm_bytes_sum"],
        "gm_bank0_plus_standard_drum_union": working_sets[
            "gm_bank0_plus_standard_drum_union"
        ],
        "greedy_16_program_union": working_sets["greedy_16_program_union"],
        "gm_15_melodic_plus_1_drum_greedy_union": working_sets[
            "gm_15_melodic_plus_1_drum_greedy_union"
        ],
        "notes": [
            "exact_worst_active_ready_union scans every distinct pair among GM bank-0 programs 0..127 plus bank-128/program-0",
            "the 4 MiB gate models the dedicated PCM residency arena, not total ESP32-S3 PSRAM",
            "16-program greedy unions are stress telemetry and are not admission requirements for one active+READY Program Change",
        ],
    }


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(
        description="Generate and validate the full-GM PCM residency pressure report"
    )
    parser.add_argument("sf2", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--arena-bytes", type=int, default=DEFAULT_ARENA_BYTES)
    args = parser.parse_args(argv)

    if args.arena_bytes <= 0:
        parser.error("--arena-bytes must be positive")

    report = build_pressure_report(args.sf2, args.arena_bytes)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    pair = report["exact_worst_active_ready_union"]
    active = pair["active_program"]
    ready = pair["ready_program"]
    required = pair["aligned_pcm_bytes"]
    margin = report["exact_pair_margin_bytes"]
    print(
        "full GM exact active+READY pressure: "
        f"bank={active['bank']} program={active['program']} {active['name']!r} + "
        f"bank={ready['bank']} program={ready['program']} {ready['name']!r} -> "
        f"{required} bytes; arena={args.arena_bytes}; margin={margin}"
    )

    if not report["exact_pair_fits_arena"]:
        print("ERROR: exact worst active+READY pair exceeds the PCM arena")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
