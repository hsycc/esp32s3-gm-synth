"""Combine one physical N16R8 M6 serial log into a checked Markdown report."""
from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path
import re
import sys
from typing import Iterable

from tools.program_prepare_benchmark.report import parse_log as parse_prepare_log
from tools.render_benchmark.report import parse_cases, validate_cases
from tools.storage_benchmark.report import parse_log as parse_storage_log

EXPECTED_VOICES = (1, 4, 8, 12, 16, 18, 20, 24)
ARENA_BYTES = 4_718_592
PSRAM_BYTES = 8_388_608
STORAGE_BYTES = 4_194_304
PLAYABLE_SAMPLES = 67
REQUIRED_VOICE = 16

_FAILURES = (
    "Guru Meditation", "assert failed", "abort()", "panic'ed",
    "PCM arena allocation failed", "Largest SPIRAM block is already smaller",
    "SampleResidencyManager failed to initialize", "Benchmark FATFS mount failed",
    "Program Prepare hardware benchmark FAILED", "Render timing benchmark FAILED",
)
_DETECTED_RE = re.compile(r"Detected physical PSRAM:\s+(\d+) bytes")
_ARENA_RE = re.compile(r"Requesting PCM residency arena:\s+(\d+) bytes")
_HEAP_RE = re.compile(
    r"(?P<kind>PSRAM|INTERNAL_HEAP) (?P<phase>before arena|after arena|after Program Prepare|after render benchmark): "
    r"total=(?P<total>\d+) free=(?P<free>\d+) largest=(?P<largest>\d+) minimum_free=(?P<minimum>\d+) bytes"
)
_FINAL_RE = re.compile(
    r"M6_QUAL_MEMORY arena_capacity=(?P<capacity>\d+) arena_used=(?P<used>\d+) "
    r"arena_high_water=(?P<high>\d+) arena_largest_free_span=(?P<largest>\d+) "
    r"resident_samples=(?P<samples>\d+) allocation_failures=(?P<alloc>\d+) stale_reference_faults=(?P<stale>\d+)"
)
_START_RE = re.compile(
    r"render_bench: START .*?budget_us=(?P<budget>\d+) timing_scope=(?P<scope>\S+) physical_timing_evidence=(?P<physical>\d+)"
)
_PASS_RE = re.compile(
    r"render_bench: PASS cases=(?P<cases>\d+) final_refs=(?P<refs>\d+) clock_faults=(?P<clock>\d+) "
    r"duration_clamps=(?P<clamps>\d+) timing_scope=(?P<scope>\S+) physical_timing_evidence=(?P<physical>\d+)"
)


@dataclass(frozen=True)
class HeapSnapshot:
    kind: str
    phase: str
    total: int
    free: int
    largest: int
    minimum_free: int


@dataclass(frozen=True)
class FinalMemory:
    capacity: int
    used: int
    high_water: int
    largest_free_span: int
    resident_samples: int
    allocation_failures: int
    stale_reference_faults: int


@dataclass(frozen=True)
class QualificationReport:
    detected_psram: int | None
    requested_arena: int | None
    psram: tuple[HeapSnapshot, ...]
    internal: tuple[HeapSnapshot, ...]
    final: FinalMemory | None
    storage: object
    prepare: object
    render_cases: tuple[object, ...]
    block_budget_us: int | None
    required_voice: int
    highest_within_deadline: int | None
    errors: tuple[str, ...]

    @property
    def passed(self) -> bool:
        return not self.errors


def _heaps(text: str) -> tuple[tuple[HeapSnapshot, ...], tuple[HeapSnapshot, ...]]:
    rows = tuple(
        HeapSnapshot(
            m.group("kind"), m.group("phase"), int(m.group("total")), int(m.group("free")),
            int(m.group("largest")), int(m.group("minimum")),
        )
        for m in _HEAP_RE.finditer(text)
    )
    return tuple(x for x in rows if x.kind == "PSRAM"), tuple(x for x in rows if x.kind == "INTERNAL_HEAP")


def _final(text: str) -> FinalMemory | None:
    m = _FINAL_RE.search(text)
    if m is None:
        return None
    return FinalMemory(*(int(m.group(name)) for name in ("capacity", "used", "high", "largest", "samples", "alloc", "stale")))


def parse_log(
    text: str, *, expected_psram: int = PSRAM_BYTES, expected_arena: int = ARENA_BYTES,
    expected_storage_bytes: int = STORAGE_BYTES, expected_samples: int = PLAYABLE_SAMPLES,
    expected_voices: Iterable[int] = EXPECTED_VOICES, required_voice: int = REQUIRED_VOICE,
) -> QualificationReport:
    errors = [f"failure signature present: {p}" for p in _FAILURES if p in text]
    detected_match, arena_match = _DETECTED_RE.search(text), _ARENA_RE.search(text)
    detected = int(detected_match.group(1)) if detected_match else None
    arena = int(arena_match.group(1)) if arena_match else None
    if detected != expected_psram:
        errors.append(f"PSRAM size mismatch: expected {expected_psram}, got {detected}")
    if arena != expected_arena:
        errors.append(f"PCM arena mismatch: expected {expected_arena}, got {arena}")

    psram, internal = _heaps(text)
    phases = {"before arena", "after arena", "after Program Prepare", "after render benchmark"}
    for name, rows in (("PSRAM", psram), ("internal heap", internal)):
        missing = phases - {row.phase for row in rows}
        if missing:
            errors.append(f"missing {name} phases: {', '.join(sorted(missing))}")

    final = _final(text)
    if final is None:
        errors.append("missing M6_QUAL_MEMORY marker")
    else:
        if final.capacity != expected_arena:
            errors.append(f"final arena capacity mismatch: expected {expected_arena}, got {final.capacity}")
        if final.used > final.capacity or final.high_water > final.capacity:
            errors.append("final arena usage/high-water exceeds capacity")
        if final.stale_reference_faults:
            errors.append(f"stale reference faults: {final.stale_reference_faults}")

    storage = parse_storage_log(text, expected_bytes=expected_storage_bytes, expected_samples=expected_samples)
    errors.extend(f"storage: {e}" for e in storage.errors)
    if storage.residency_capacity is not None and storage.residency_capacity != expected_arena:
        errors.append(f"storage/residency arena mismatch: expected {expected_arena}, got {storage.residency_capacity}")

    prepare = parse_prepare_log(text, expected_samples=expected_samples)
    errors.extend(f"prepare: {e}" for e in prepare.errors)
    if prepare.arena_bytes is not None and prepare.arena_bytes != expected_arena:
        errors.append(f"Program Prepare arena mismatch: expected {expected_arena}, got {prepare.arena_bytes}")

    expected_voice_tuple = tuple(expected_voices)
    cases: tuple[object, ...] = ()
    try:
        cases = tuple(validate_cases(text, parse_cases(text), expected_voice_tuple))
    except ValueError as exc:
        errors.append(f"render: {exc}")

    start, passed = _START_RE.search(text), _PASS_RE.search(text)
    budget = int(start.group("budget")) if start else None
    for label, marker in (("START", start), ("PASS", passed)):
        if marker is None:
            errors.append(f"missing render {label} physical-evidence marker")
        elif marker.group("scope") != "hardware" or marker.group("physical") != "1":
            errors.append(f"render {label} is not marked as physical hardware evidence")
    if passed is not None:
        if int(passed.group("cases")) != len(expected_voice_tuple):
            errors.append("render PASS case count does not match expected Voice matrix")
        if any(int(passed.group(name)) for name in ("refs", "clock", "clamps")):
            errors.append("render PASS reports refs/clock faults/duration clamps")

    highest = None
    if cases and budget is not None:
        for case in cases:
            ok = case.deadline_misses == 0 and case.max_us <= budget
            if ok:
                highest = case.voices
            if case.voices <= required_voice and not ok:
                errors.append(
                    f"required Voice case {case.voices} misses deadline: max={case.max_us} us "
                    f"misses={case.deadline_misses} budget={budget} us"
                )
    if required_voice not in expected_voice_tuple:
        errors.append(f"required Voice count {required_voice} is not in expected matrix")
    if "M6 bring-up core linked successfully" not in text:
        errors.append("missing final M6 bring-up success marker")

    return QualificationReport(detected, arena, psram, internal, final, storage, prepare, cases, budget, required_voice, highest, tuple(errors))


def _heap_table(lines: list[str], rows: Iterable[HeapSnapshot]) -> None:
    lines += ["| Phase | Total | Free | Largest | Minimum free |", "|---|---:|---:|---:|---:|"]
    lines += [f"| {r.phase} | {r.total} | {r.free} | {r.largest} | {r.minimum_free} |" for r in rows]


def render_markdown(report: QualificationReport, *, title: str = "ESP32-S3 N16R8 M6 Physical Qualification") -> str:
    lines = [
        f"# {title}", "", f"Status: **{'PASS' if report.passed else 'FAIL'}**", "",
        "> This validates one physical qualification log. It does not by itself prove final I2S underrun behavior or guaranteed product polyphony.", "",
        "## Configuration", "", f"- detected PSRAM: `{report.detected_psram}` bytes",
        f"- requested PCM arena: `{report.requested_arena}` bytes",
        f"- 64-frame render budget: `{report.block_budget_us}` us",
        f"- required zero-miss Voice case: `{report.required_voice}`",
        f"- highest observed within-deadline Voice case: `{report.highest_within_deadline}`", "", "## PSRAM heap", "",
    ]
    _heap_table(lines, report.psram)
    lines += ["", "## Internal SRAM heap", ""]
    _heap_table(lines, report.internal)
    if report.final:
        f = report.final
        lines += ["", "## Final residency", "", f"- capacity / used / high-water: `{f.capacity}` / `{f.used}` / `{f.high_water}` bytes",
                  f"- largest free arena span: `{f.largest_free_span}` bytes", f"- resident samples: `{f.resident_samples}`",
                  f"- allocation failures: `{f.allocation_failures}`", f"- stale-reference faults: `{f.stale_reference_faults}`"]

    lines += ["", "## Flash -> PSRAM", "", "| Chunk | MiB/s | P95 us | P99 us | Max us |", "|---:|---:|---:|---:|---:|"]
    for r in report.storage.chunks:
        lines.append(f"| {r.chunk_size} | {r.throughput_mib_s:.3f} | {r.p95_us} | {r.p99_us} | {r.max_us} |")

    lines += ["", "## Program Prepare", "", "| Case | Elapsed us | Loaded bytes | Reused | Evictions | Evicted bytes |",
              "|---|---:|---:|---:|---:|---:|"]
    for c in (report.prepare.cold, report.prepare.warm, report.prepare.eviction):
        if c:
            lines.append(f"| {c.name} | {c.elapsed_us} | {c.loaded_bytes} | {c.reused_resident} | {c.evictions} | {c.evicted_bytes} |")

    lines += ["", "## Voice DSP render", "", "| Voices | P95 us | P99 us | Max us | Margin us | Misses | Observed |",
              "|---:|---:|---:|---:|---:|---:|---|"]
    for c in report.render_cases:
        margin = (report.block_budget_us or 0) - c.max_us
        state = "within deadline" if c.deadline_misses == 0 and margin >= 0 else "deadline miss"
        lines.append(f"| {c.voices} | {c.p95_us} | {c.p99_us} | {c.max_us} | {margin} | {c.deadline_misses} | {state} |")
    lines += ["", "The highest within-deadline case is only a short deterministic hardware result. Final qualification still requires the real I2S/DMA path and long-running mixed-MIDI stress."]
    if report.errors:
        lines += ["", "## Validation errors", ""] + [f"- {e}" for e in report.errors]
    return "\n".join(lines) + "\n"


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("log", type=Path)
    p.add_argument("--output", type=Path)
    p.add_argument("--title", default="ESP32-S3 N16R8 M6 Physical Qualification")
    p.add_argument("--expected-psram", type=int, default=PSRAM_BYTES)
    p.add_argument("--expected-arena", type=int, default=ARENA_BYTES)
    p.add_argument("--expected-storage-bytes", type=int, default=STORAGE_BYTES)
    p.add_argument("--expected-samples", type=int, default=PLAYABLE_SAMPLES)
    p.add_argument("--required-voice", type=int, default=REQUIRED_VOICE)
    args = p.parse_args(argv)
    text = args.log.read_text(encoding="utf-8", errors="replace")
    report = parse_log(text, expected_psram=args.expected_psram, expected_arena=args.expected_arena,
                       expected_storage_bytes=args.expected_storage_bytes, expected_samples=args.expected_samples,
                       required_voice=args.required_voice)
    markdown = render_markdown(report, title=args.title)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(markdown, encoding="utf-8")
    else:
        sys.stdout.write(markdown)
    return 0 if report.passed else 2


if __name__ == "__main__":
    raise SystemExit(main())
