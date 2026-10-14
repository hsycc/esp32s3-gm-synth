from __future__ import annotations

from collections import Counter
from typing import Dict, List

from .core import GEN_NAMES, Sf2
from .resolved_zone import PRESET_FORBIDDEN


STANDARD_GENERATOR_OPS = set(GEN_NAMES)
STRUCTURAL_GENERATORS = {41, 43, 44, 53, 60}


def _used_ops(sf2: Sf2):
    preset = Counter(g.oper for g in sf2.pgen[:-1])
    instrument = Counter(g.oper for g in sf2.igen[:-1])
    return preset, instrument


def make_compatibility_report(sf2: Sf2) -> dict:
    preset_ops, instrument_ops = _used_ops(sf2)
    all_ops = set(preset_ops) | set(instrument_ops)
    unknown_ops = sorted(op for op in all_ops if op not in STANDARD_GENERATOR_OPS)
    invalid_preset_ops = sorted(op for op in preset_ops if op in PRESET_FORBIDDEN)

    sm24_present = ("sdta", "sm24") in sf2.index
    rom_samples = [
        i for i, sample in enumerate(sf2.shdr[:-1]) if sample.is_rom
    ]
    stereo_or_linked = [
        i
        for i, sample in enumerate(sf2.shdr[:-1])
        if sample.base_type in (2, 4, 8)
    ]

    pmods = sf2.nonzero_mods(sf2.pmod)
    imods = sf2.nonzero_mods(sf2.imod)

    unsupported: List[str] = []
    warnings: List[str] = []
    planned: List[str] = []

    if sm24_present:
        unsupported.append("sm24_24bit_sample_payload")
    if rom_samples:
        unsupported.append("rom_sample_types")
    if unknown_ops:
        warnings.append("unknown_generator_operators")
    if invalid_preset_ops:
        warnings.append("invalid_generators_present_at_preset_level")
    if stereo_or_linked:
        planned.append("stereo_linked_sample_runtime")
    if pmods or imods:
        planned.append("custom_pmod_imod_semantics")

    # Default modulators matter even when pmod/imod chunks contain no effective
    # custom records. They are part of the target SF2 compatibility profile.
    planned.append("default_sf2_modulators")

    if unsupported:
        import_class = "unsupported_features_present"
    elif unknown_ops:
        import_class = "unknown_generator_semantics_present"
    elif planned:
        import_class = "parseable_but_runtime_qualification_incomplete"
    else:
        import_class = "parseable_current_profile"

    return {
        "import_class": import_class,
        "features": {
            "riff_sfbk": "implemented",
            "sf2_16bit_smpl": "implemented",
            "generator_table_parsing": "implemented",
            "preset_instrument_global_local_hierarchy": "host_resolver_implemented",
            "sample_address_loop_resolution": "host_resolver_implemented",
            "default_sf2_modulators": "planned_m2_5",
            "custom_imod": "planned_m2_5" if imods else "not_exercised_by_bank",
            "custom_pmod": "planned_m2_5" if pmods else "not_exercised_by_bank",
            "stereo_linked_samples": (
                "planned_runtime_voice_support"
                if stereo_or_linked
                else "not_exercised_by_bank"
            ),
            "sm24_24bit": "unsupported_v1" if sm24_present else "not_present",
            "rom_samples": "unsupported_v1" if rom_samples else "not_present",
            "sf3_vorbis": "out_of_scope_v1",
        },
        "generators": {
            "preset_usage": {
                GEN_NAMES.get(op, f"oper_{op}"): count
                for op, count in sorted(preset_ops.items())
            },
            "instrument_usage": {
                GEN_NAMES.get(op, f"oper_{op}"): count
                for op, count in sorted(instrument_ops.items())
            },
            "unknown_operators": unknown_ops,
            "invalid_preset_level_operators": [
                {
                    "oper": op,
                    "name": GEN_NAMES.get(op, f"oper_{op}"),
                    "count": preset_ops[op],
                }
                for op in invalid_preset_ops
            ],
        },
        "modulators": {
            "pmod_effective_record_count": len(pmods),
            "imod_effective_record_count": len(imods),
            "pmod_amount_nonzero_count": sum(1 for m in pmods if m["amount"] != 0),
            "imod_amount_nonzero_count": sum(1 for m in imods if m["amount"] != 0),
        },
        "samples": {
            "sm24_present": sm24_present,
            "rom_sample_ids": rom_samples,
            "stereo_or_linked_sample_ids": stereo_or_linked,
        },
        "unsupported_features": unsupported,
        "planned_features_exercised": sorted(set(planned)),
        "warnings": warnings,
        "notes": [
            "parseable does not mean audio-runtime qualified",
            "zero-amount instrument modulators may intentionally disable default modulators and must not be discarded blindly",
            "non-standard or malformed preset-level generators are reported instead of silently becoming product requirements",
        ],
    }
