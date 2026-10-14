from __future__ import annotations

import argparse
import json
from collections import Counter, defaultdict
from pathlib import Path

from tools.sf2_conformance.compare_case import compare_case


DEFAULT_FIXTURES = (
    Path("assets/sf2/subsets/FM_GM_Open_subset_6MiB.sf2"),
    Path("assets/sf2/subsets/GeneralUser_GS_subset_6MiB.sf2"),
    Path("assets/sf2/subsets/FluidR3_GM_subset_6MiB.sf2"),
)

# These programs are intentionally present in all three comparable fixtures and
# cover a mix of simple sample playback, sustained instruments and synth voices.
DEFAULT_PROGRAMS = (8, 16, 32, 50, 56, 80, 88, 96)


def main() -> int:
    parser = argparse.ArgumentParser(description="Run real-SF2 generator conformance cases")
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--oracle-version", default="FluidSynth pinned source")
    parser.add_argument("--oracle-sha", default="unknown")
    parser.add_argument("--fixture", type=Path, action="append", dest="fixtures")
    parser.add_argument("--program", type=int, action="append", dest="programs")
    parser.add_argument("--key", type=int, default=60)
    parser.add_argument("--velocity", type=int, default=96)
    parser.add_argument("--tolerance", type=float, default=1e-3)
    parser.add_argument("--output", type=Path, default=Path("reports/sf2_conformance.json"))
    parser.add_argument("--strict-generators", action="store_true")
    args = parser.parse_args()

    fixtures = tuple(args.fixtures or DEFAULT_FIXTURES)
    programs = tuple(args.programs or DEFAULT_PROGRAMS)

    cases = []
    exceptions = []
    for fixture in fixtures:
        for program in programs:
            try:
                report = compare_case(
                    args.probe,
                    fixture,
                    bank=0,
                    program=program,
                    key=args.key,
                    velocity=args.velocity,
                    tolerance=args.tolerance,
                )
                cases.append(report)
                print(
                    f"{fixture.name:34s} P{program:03d} "
                    f"voices={report['voice_count']['ours']}/{report['voice_count']['fluidsynth']} "
                    f"keyvel={report['key_velocity_mismatch_count']} "
                    f"gen={report['generator_mismatch_count']} "
                    f"max_delta={report['max_abs_generator_delta']:.6g}"
                )
            except Exception as exc:  # Keep the suite diagnostic on the first pass.
                exceptions.append(
                    {
                        "fixture": str(fixture),
                        "program": program,
                        "key": args.key,
                        "velocity": args.velocity,
                        "error": f"{type(exc).__name__}: {exc}",
                    }
                )
                print(f"ERROR {fixture.name} P{program:03d}: {type(exc).__name__}: {exc}")

    mismatch_by_generator = Counter()
    max_delta_by_generator = defaultdict(float)
    for case in cases:
        for pair in case["pairs"]:
            for name, item in pair["generators"].items():
                if not item["match"]:
                    mismatch_by_generator[name] += 1
                max_delta_by_generator[name] = max(
                    max_delta_by_generator[name], abs(float(item["delta"]))
                )

    structural_failures = [case for case in cases if not case["structural_ok"]]
    generator_failures = [case for case in cases if not case["generators_ok"]]
    summary = {
        "case_count": len(cases),
        "exception_count": len(exceptions),
        "structural_failure_count": len(structural_failures),
        "generator_failure_count": len(generator_failures),
        "mismatch_by_generator": dict(sorted(mismatch_by_generator.items())),
        "max_abs_delta_by_generator": dict(sorted(max_delta_by_generator.items())),
    }
    output = {
        "schema_version": 2,
        "oracle": {
            "implementation": args.oracle_version,
            "source_sha": args.oracle_sha,
            "generator_accessor": "fluid_voice_gen_value",
            "semantics": "generator.val + generator.mod + generator.nrpn",
        },
        "fixtures": [str(f) for f in fixtures],
        "programs": list(programs),
        "key": args.key,
        "velocity": args.velocity,
        "tolerance": args.tolerance,
        "summary": summary,
        "exceptions": exceptions,
        "cases": cases,
    }

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(output, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(json.dumps(summary, sort_keys=True))

    if exceptions or structural_failures:
        return 1
    if args.strict_generators and generator_failures:
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
