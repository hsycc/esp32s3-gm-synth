from pathlib import Path

from .core import Sf2, Sf2Error
from .modulator import (
    Modulator,
    build_mod_plan,
    compile_note_mod_plans,
    decode_source,
    default_modulators,
    encode_source,
    merge_modulators,
)
from .report import make_report
from .resolved_zone import find_preset_index, resolve_note, resolve_preset_zones


def analyze(path: Path, psram_bytes=None, reserve_bytes=None):
    return make_report(Sf2(Path(path)).parse(), psram_bytes, reserve_bytes)


__all__ = [
    "Sf2",
    "Sf2Error",
    "Modulator",
    "analyze",
    "make_report",
    "find_preset_index",
    "resolve_note",
    "resolve_preset_zones",
    "encode_source",
    "decode_source",
    "default_modulators",
    "merge_modulators",
    "build_mod_plan",
    "compile_note_mod_plans",
]
