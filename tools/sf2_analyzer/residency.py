from __future__ import annotations

from typing import Optional, Sequence

from .core import Sf2, align4


def _identity(program: dict) -> dict:
    return {
        "bank": int(program["bank"]),
        "program": int(program["program"]),
        "name": program.get("name", ""),
    }


def exact_worst_active_ready_pair(
    sf2: Sf2,
    programs: Sequence[dict],
) -> Optional[dict]:
    """Return the exact largest two-program PCM union.

    Program Prepare keeps the ACTIVE generation pinned until the READY
    generation commits and is ACKed. For a fixed bank, the minimum arena
    requirement for an arbitrary Program Change is therefore the largest union
    of sample dependencies for any two distinct program identities.

    This is exact for the supplied program list (O(N^2)); unlike the existing
    16-program greedy stress estimator it is not a heuristic.
    """
    if len(programs) < 2:
        return None

    aligned_bytes = [align4(sample.pcm_bytes) for sample in sf2.shdr[:-1]]
    sample_sets = [set(program["sample_ids"]) for program in programs]

    best = None
    best_key = None
    for left_index in range(len(programs) - 1):
        left = programs[left_index]
        left_samples = sample_sets[left_index]
        for right_index in range(left_index + 1, len(programs)):
            right = programs[right_index]
            right_samples = sample_sets[right_index]
            union_ids = left_samples | right_samples
            shared_ids = left_samples & right_samples
            union_bytes = sum(aligned_bytes[sample_id] for sample_id in union_ids)
            shared_bytes = sum(aligned_bytes[sample_id] for sample_id in shared_ids)

            # Stable identity tie-break keeps generated reports deterministic
            # even when several pairs have exactly the same PCM footprint.
            identity_key = (
                int(left["bank"]),
                int(left["program"]),
                int(right["bank"]),
                int(right["program"]),
            )
            candidate_key = (union_bytes, tuple(-value for value in identity_key))
            if best_key is not None and candidate_key <= best_key:
                continue

            best_key = candidate_key
            best = {
                "method": "exact_all_distinct_program_pairs",
                "program_count": 2,
                "active_program": _identity(left),
                "ready_program": _identity(right),
                "unique_sample_count": len(union_ids),
                "shared_sample_count": len(shared_ids),
                "sample_ids": sorted(union_ids),
                "aligned_pcm_bytes": union_bytes,
                "shared_aligned_pcm_bytes": shared_bytes,
                "active_aligned_pcm_bytes": int(left["aligned_pcm_bytes"]),
                "ready_aligned_pcm_bytes": int(right["aligned_pcm_bytes"]),
            }

    return best


def gm_programs_for_transition_pressure(programs: Sequence[dict]) -> list[dict]:
    """Return the GM transition domain: bank-0 0..127 + standard drum preset.

    SoundFont PHDR ordering is authoritative for duplicate bank/program
    identities, so preserve the first occurrence exactly as the firmware
    resolvers do rather than allowing a later duplicate to replace it.
    """
    melodic_by_program: dict[int, dict] = {}
    for program in programs:
        if int(program["bank"]) != 0:
            continue
        program_number = int(program["program"])
        if 0 <= program_number <= 127:
            melodic_by_program.setdefault(program_number, program)

    melodic = [melodic_by_program[index] for index in sorted(melodic_by_program)]
    standard_drum = next(
        (
            program
            for program in programs
            if int(program["bank"]) == 128 and int(program["program"]) == 0
        ),
        None,
    )
    if standard_drum is not None:
        melodic.append(standard_drum)
    return melodic
