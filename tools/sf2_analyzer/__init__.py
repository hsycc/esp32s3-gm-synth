from pathlib import Path

from .core import Sf2, Sf2Error
from .report import make_report
from .resolved_zone import find_preset_index, resolve_note, resolve_preset_zones


def analyze(path: Path, psram_bytes=None, reserve_bytes=None):
    return make_report(Sf2(Path(path)).parse(), psram_bytes, reserve_bytes)


__all__ = [
    "Sf2",
    "Sf2Error",
    "analyze",
    "make_report",
    "find_preset_index",
    "resolve_note",
    "resolve_preset_zones",
]
