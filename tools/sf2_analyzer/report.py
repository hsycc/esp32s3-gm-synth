from __future__ import annotations

import hashlib
from collections import Counter
from typing import Optional

from .compatibility import make_compatibility_report
from .core import SAMPLE_TYPE_NAMES, Sf2, align4


def _union(programs, samples):
    ids = set()
    for p in programs:
        ids.update(p["sample_ids"])
    return {
        "program_count": len(programs),
        "unique_sample_count": len(ids),
        "sample_ids": sorted(ids),
        "pcm_bytes": sum(samples[i].pcm_bytes for i in ids),
        "aligned_pcm_bytes": sum(align4(samples[i].pcm_bytes) for i in ids),
    }


def _greedy(sf2: Sf2, programs, count=16, seed=()):
    selected = list(seed)
    ids = set()
    for p in selected:
        ids.update(p["sample_ids"])
    remaining = [p for p in programs if p not in selected]
    while remaining and len(selected) < count:
        def gain(p):
            new = set(p["sample_ids"]) - ids
            return (
                sum(align4(sf2.shdr[i].pcm_bytes) for i in new),
                p["aligned_pcm_bytes"],
            )

        best = max(remaining, key=gain)
        if selected and gain(best)[0] == 0:
            break
        selected.append(best)
        ids.update(best["sample_ids"])
        remaining.remove(best)
    out = _union(selected, sf2.shdr)
    out["method"] = "greedy_max_incremental_aligned_pcm_not_exact_global_optimum"
    out["programs"] = [
        {"bank": p["bank"], "program": p["program"], "name": p["name"]}
        for p in selected
    ]
    return out


def _mod_summary(records):
    amount_nonzero = sum(1 for item in records if item["amount"] != 0)
    return {
        "record_count": len(records),
        "amount_nonzero_count": amount_nonzero,
        "zero_amount_record_count": len(records) - amount_nonzero,
        "items": records,
    }


def make_report(sf2: Sf2, psram_bytes: Optional[int] = None, reserve_bytes: Optional[int] = None):
    programs = [sf2.resolve_preset(i) for i in range(max(0, len(sf2.phdr) - 1))]
    samples = []
    linked = stereo = rom = 0
    sample_rates = Counter()
    for i, s in enumerate(sf2.shdr[:-1]):
        base = s.base_type
        linked += base in (2, 4, 8)
        stereo += base in (2, 4)
        rom += s.is_rom
        sample_rates[s.rate] += 1
        samples.append(
            {
                "sample_id": i,
                "name": s.name,
                "pcm_bytes": s.pcm_bytes,
                "aligned_pcm_bytes": align4(s.pcm_bytes),
                "sample_rate": s.rate,
                "original_pitch": s.root,
                "pitch_correction": s.correction,
                "sample_link": s.link,
                "sample_type": s.type,
                "sample_type_name": SAMPLE_TYPE_NAMES.get(base, f"type_{base}"),
                "is_rom": s.is_rom,
            }
        )
    aligned = sum(x["aligned_pcm_bytes"] for x in samples)
    raw = sum(x["pcm_bytes"] for x in samples)
    melodic = [p for p in programs if p["bank"] == 0]
    drums = [p for p in programs if p["bank"] == 128]
    gm16 = []
    for d in drums:
        u = _greedy(sf2, melodic, 16, [d])
        u["drum_program"] = {
            "bank": d["bank"],
            "program": d["program"],
            "name": d["name"],
        }
        gm16.append(u)
    standard_drum = next((p for p in drums if p["program"] == 0), None)
    pmods = sf2.nonzero_mods(sf2.pmod)
    imods = sf2.nonzero_mods(sf2.imod)
    residency = {
        "target_psram_bytes": psram_bytes,
        "reserve_bytes": reserve_bytes,
        "available_for_pcm_bytes": None,
        "all_samples_aligned_bytes": aligned,
        "fits_all_samples": None,
    }
    if psram_bytes is not None:
        available = max(0, psram_bytes - (reserve_bytes or 0))
        residency.update(
            available_for_pcm_bytes=available,
            fits_all_samples=aligned <= available,
        )
    bank_counts = Counter(p["bank"] for p in programs)

    return {
        "schema_version": 3,
        "file": {
            "path": str(sf2.path),
            "size_bytes": sf2.file_size,
            "sha256": hashlib.sha256(sf2.data).hexdigest(),
        },
        "riff": {"form": "sfbk", "chunks": sf2.chunk_summary()},
        "sdta": {
            "smpl_payload_bytes": sf2.smpl_bytes,
            "smpl_sample_points": sf2.smpl_bytes // 2,
        },
        "counts": {
            "presets": max(0, len(sf2.phdr) - 1),
            "instruments": max(0, len(sf2.inst) - 1),
            "samples": max(0, len(sf2.shdr) - 1),
            "pbag_records": len(sf2.pbag),
            "ibag_records": len(sf2.ibag),
            "pgen_records": len(sf2.pgen),
            "igen_records": len(sf2.igen),
            "pmod_records": len(sf2.pmod),
            "imod_records": len(sf2.imod),
        },
        "banks": {
            "program_counts": {
                str(bank): count for bank, count in sorted(bank_counts.items())
            }
        },
        "generators": {
            "preset_usage": sf2.generator_usage(sf2.pbag, sf2.pgen),
            "instrument_usage": sf2.generator_usage(sf2.ibag, sf2.igen),
        },
        "modulators": {
            "pmod_nonzero_count": len(pmods),
            "imod_nonzero_count": len(imods),
            "pmod_amount_nonzero_count": sum(
                1 for item in pmods if item["amount"] != 0
            ),
            "imod_amount_nonzero_count": sum(
                1 for item in imods if item["amount"] != 0
            ),
            "pmod_summary": _mod_summary(pmods),
            "imod_summary": _mod_summary(imods),
            "pmod": pmods,
            "imod": imods,
        },
        "samples": {
            "raw_pcm_bytes_sum": raw,
            "aligned_pcm_bytes_sum": aligned,
            "linked_sample_count": int(linked),
            "stereo_side_sample_count": int(stereo),
            "rom_sample_count": int(rom),
            "sample_rate_distribution": {
                str(rate): count for rate, count in sorted(sample_rates.items())
            },
            "items": samples,
        },
        "programs": programs,
        "gm": {
            "bank0_program_count": len(melodic),
            "bank128_drum_preset_count": len(drums),
            "drum_presets": [
                {
                    "bank": p["bank"],
                    "program": p["program"],
                    "name": p["name"],
                    "unique_sample_count": p["unique_sample_count"],
                    "aligned_pcm_bytes": p["aligned_pcm_bytes"],
                    "max_overlapping_zones": p["max_overlapping_zones"],
                }
                for p in drums
            ],
        },
        "working_sets": {
            "all_presets_union": _union(programs, sf2.shdr),
            "gm_bank0_union": _union(melodic, sf2.shdr),
            "gm_bank0_plus_standard_drum_union": _union(
                melodic + ([standard_drum] if standard_drum else []), sf2.shdr
            ),
            "greedy_16_program_union": _greedy(sf2, melodic, 16),
            "gm_15_melodic_plus_1_drum_greedy_union": max(
                gm16, key=lambda x: x["aligned_pcm_bytes"], default=None
            ),
        },
        "residency": residency,
        "compatibility": make_compatibility_report(sf2),
        "notes": [
            "aligned residency sums each referenced sample independently at 4-byte alignment",
            "greedy 16-program unions are heuristic stress estimates, not exact combinatorial maxima",
            "bank 128 is reported as the conventional GM drum bank; banks may use other conventions",
            "modulator record_count includes non-terminal zero-amount records because they may override/disable default modulators",
            "modulator amount_nonzero_count reports only records whose modulation amount is non-zero",
            "compatibility parseability does not imply runtime DSP qualification",
        ],
    }
