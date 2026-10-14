from __future__ import annotations

import argparse
import json
from pathlib import Path

from .sf2_subset import SoundFont


def choose_diverse_melodic_subset(sf2: SoundFont, target_bytes: int):
    """Select a cross-bank-comparable melodic GM fixture near target size.

    Common ~6 MiB fixtures intentionally exclude drums. A large Standard Drum
    kit (notably FluidR3_GM) can exceed the entire comparison budget by itself,
    which would make the three fixtures incomparable. Drum kits are exercised
    separately as dedicated stress assets.
    """
    selected = []
    candidates = []
    for offset in range(8):
        for family in range(16):
            program = family * 8 + offset
            preset_index = sf2.find_preset(0, program)
            if preset_index is not None:
                candidates.append(preset_index)

    # Keep conservative metadata headroom; build_subset reports exact final size.
    budget = max(256 * 1024, target_bytes - 128 * 1024)
    for preset_index in candidates:
        trial = selected + [preset_index]
        if sf2.estimated_bytes(trial) <= budget:
            selected.append(preset_index)
    return selected


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Build a comparable melodic GM SF2 fixture near a byte target"
    )
    parser.add_argument("sf2", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--target-mib", type=float, default=6.0)
    parser.add_argument("--manifest", type=Path)
    args = parser.parse_args(argv)

    target_bytes = int(args.target_mib * 1024 * 1024)
    sf2 = SoundFont(args.sf2).parse()
    selected = choose_diverse_melodic_subset(sf2, target_bytes)
    manifest = sf2.build_subset(selected, args.output)
    manifest.update(
        {
            "target_mib": args.target_mib,
            "target_bytes": target_bytes,
            "selection_method": "melodic_gm_family_round_robin_no_drums",
            "standard_drum_included": False,
            "drum_policy": (
                "Common size-matched fixtures exclude drums. Drum kits are tested "
                "separately because a single large kit can exceed the 6 MiB budget."
            ),
        }
    )

    if manifest["size_bytes"] > target_bytes:
        raise RuntimeError(
            f"generated fixture exceeds target: {manifest['size_bytes']} > {target_bytes}"
        )

    text = json.dumps(manifest, indent=2, ensure_ascii=False)
    if args.manifest:
        args.manifest.parent.mkdir(parents=True, exist_ok=True)
        args.manifest.write_text(text + "\n", encoding="utf-8")
    print(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
