from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path
import re
import sys
from typing import Iterable

from tools.program_prepare_benchmark.report import BenchmarkReport as PrepareReport
from tools.program_prepare_benchmark.report import parse_log as parse_prepare_log
from tools.render_benchmark.report import RenderCase, parse_cases, validate_cases
from tools.storage_benchmark.report import BenchmarkReport as StorageReport
from tools.storage_benchmark.report import parse_log as parse_storage_log

EXPECTED_VOICES = (1, 4, 8, 12, 16, 18, 20, 24)
DEFAULT_ARENA_BYTES = 4718592
DEFAULT_STORAGE_BYTES = 4194304
DEFAULT_SAMPLE_COUNT = 67
DEFAULT_TARGET_VOICES = 16
DEFAULT_BLOCK_BUDGET_US = 1451

_FAILURE_PATTERNS = (
    "Guru Meditation",
    "assert failed",
    "abort()",
    "panic'ed",
    "PCM arena allocation failed",
    "Largest SPIRAM block is already smaller than requested arena",
    "SampleResidencyManager failed to initialize over PSRAM arena",
    "Program Prepare hardware benchmark FAILED",
    "Render timing benchmark FAILED",
)

_PSRAM_RE = re.compile(
    r"PSRAM (?P<phase>before arena|after arena|after Program Prepare|after render benchmark): "
    r"total=(?P<total>\d+) free=(?P<free>\d+) largest=(?P<largest>\d+) "
    r"minimum_free=(?P<minimum>\d+) bytes"
)
_INTERNAL_RE = re.compile(
    r"INTERNAL_HEAP (?P<phase>before arena|after arena|after Program Prepare|after render benchmark): "
    r"total=(?P<total>\d+) free=(?P<free>\d+) largest=(?P<largest>\d+) "
    r"minimum_free=(?P<minimum>\d+) bytes"
)
_ARENA_REQUEST_RE = re.compile(r"Requesting PCM residency arena: (?P<bytes>\d+) bytes")
_RENDER_START_RE = re.compile(
    r"render_bench: START .*?budget_us=(?P<budget>\d+) "
    r"timing_scope=(?P<scope>\S+) physical_timing_evidence=(?P<physical>[01])"
)
_RENDER_PASS_RE = re.compile(
    r"render_bench: PASS cases=(?P<cases>\d+) final_refs=(?P<refs>\d+) "
    r"clock_faults=(?P<clock_faults>\d+) duration_clamps=(?P<duration_clamps>\d+) "
    r"timing_scope=(?P<scope>\S+) physical_timing_evidence=(?P<physical>[01])"
)
_M6_MEMORY_RE = re.compile(
    r"M6_QUAL_MEMORY arena_capacity=(?P<capacity>\d+) arena_used=(?P<used>\d+) "
    r"arena_high_water=(?P<high_water>\d+) "
    r"arena_largest_free_span=(?P<largest_free_span>\d+) "
    r"resident_samples=(?P<resident_samples>\d+) "
    r"allocation_failures=(?P<allocation_failures>\d+) "
    r"stale_reference_faults=(?P<stale_reference_faults>\d+)"
)


@dataclass(frozen=True)
class HeapSnapshot:
    total_bytes: int
    free_bytes: int
    largest_free_block_bytes: int
    minimum_free_bytes: int


@dataclass(frozen=True)
class M6Memory:
    capacity_bytes: int
    used_bytes: int
    high_water_bytes: int
    largest_free_span_bytes: int
    resident_samples: int
    allocation_failures: int
    stale_reference_faults: int


@dataclass(frozen=True)
class QualificationReport:
    storage: StorageReport
    prepare: PrepareReport
    render_cases: tuple[RenderCase, ...]
    psram: dict[str, HeapSnapshot]
    internal_heap: dict[str, HeapSnapshot]
    memory: M6Memory | None
    block_budget_us: int | None
    target_voices: int
    guaranteed_candidate_voices: int
    errors: tuple[str, ...]

    @property
    def passed(self) -> bool:
        return not self.errors

    @property
    def target_case(self) -> RenderCase | None:
        return next((case for case in self.render_cases if case.voices == self.target_voices), None)


def _heap_snapshots(pattern: re.Pattern[str], text: str) -> dict[str, HeapSnapshot]:
    snapshots: dict[str, HeapSnapshot] = {}
    for match in pattern.finditer(text):
        snapshots[match.group("phase")] = HeapSnapshot(
            total_bytes=int(match.group("total")),
            free_bytes=int(match.group("free")),
            largest_free_block_bytes=int(match.group("largest")),
            minimum_free_bytes=int(match.group("minimum")),
        )
    return snapshots


def _case_meets_realtime_gate(case: RenderCase, budget_us: int) -> bool:
    return (
        case.deadline_misses == 0
        and case.clipped_samples == 0
        and case.max_us <= budget_us
    )


def _guaranteed_candidate(cases: Iterable[RenderCase], budget_us: int) -> int:
    candidate = 0
    for case in cases:
        if not _case_meets_realtime_gate(case, budget_us):
            break
        candidate = case.voices
    return candidate


def parse_log(
    text: str,
    *,
    expected_arena_bytes: int = DEFAULT_ARENA_BYTES,
    expected_storage_bytes: int = DEFAULT_STORAGE_BYTES,
    expected_samples: int = DEFAULT_SAMPLE_COUNT,
    target_voices: int = DEFAULT_TARGET_VOICES,
    expected_voices: Iterable[int] = EXPECTED_VOICES,
) -> QualificationReport:
    errors: list[str] = []

    for signature in _FAILURE_PATTERNS:
        if signature in text:
            errors.append(f"failure signature present: {signature}")

    storage = parse_storage_log(
        text,
        expected_bytes=expected_storage_bytes,
        expected_samples=expected_samples,
    )
    errors.extend(f"storage: {error}" for error in storage.errors)

    prepare = parse_prepare_log(text, expected_samples=expected_samples)
    errors.extend(f"prepare: {error}" for error in prepare.errors)

    expected_voice_list = list(expected_voices)
    render_cases: list[RenderCase] = []
    try:
        render_cases = validate_cases(text, parse_cases(text), expected_voice_list)
    except ValueError as exc:
        errors.append(f"render: {exc}")

    start = _RENDER_START_RE.search(text)
    render_pass = _RENDER_PASS_RE.search(text)
    block_budget_us = int(start.group("budget")) if start else None
    if start is None:
        errors.append("render: missing START physical-evidence marker")
    elif start.group("scope") != "hardware" or start.group("physical") != "1":
        errors.append("render: timing is not marked as physical hardware evidence")

    if render_pass is None:
        errors.append("render: missing PASS physical-evidence marker")
    else:
        if render_pass.group("scope") != "hardware" or render_pass.group("physical") != "1":
            errors.append("render: PASS is not marked as physical hardware evidence")
        if int(render_pass.group("cases")) != len(expected_voice_list):
            errors.append(
                f"render: PASS case count mismatch: expected {len(expected_voice_list)}, "
                f"got {render_pass.group('cases')}"
            )
        if int(render_pass.group("refs")) != 0:
            errors.append("render: final Voice refs are not zero")
        if int(render_pass.group("clock_faults")) != 0:
            errors.append("render: realtime clock faults are non-zero")
        if int(render_pass.group("duration_clamps")) != 0:
            errors.append("render: duration clamps are non-zero")

    if block_budget_us is not None:
        if block_budget_us != DEFAULT_BLOCK_BUDGET_US:
            errors.append(
                f"render: unexpected 64-frame budget: expected "
                f"{DEFAULT_BLOCK_BUDGET_US}, got {block_budget_us}"
            )
        for case in render_cases:
            if case.clipped_samples != 0:
                errors.append(
                    f"render: clipped samples at {case.voices} voices: "
                    f"{case.clipped_samples}"
                )

    target_case = next((case for case in render_cases if case.voices == target_voices), None)
    if target_case is None:
        errors.append(f"render: missing target Voice case: {target_voices}")
    elif block_budget_us is not None and not _case_meets_realtime_gate(
        target_case, block_budget_us
    ):
        errors.append(
            f"render: {target_voices}-Voice target missed realtime gate "
            f"(max_us={target_case.max_us}, deadline_misses={target_case.deadline_misses}, "
            f"clipped_samples={target_case.clipped_samples}, budget_us={block_budget_us})"
        )

    psram = _heap_snapshots(_PSRAM_RE, text)
    internal_heap = _heap_snapshots(_INTERNAL_RE, text)
    required_phases = (
        "before arena",
        "after arena",
        "after Program Prepare",
        "after render benchmark",
    )
    for phase in required_phases:
        if phase not in psram:
            errors.append(f"memory: missing PSRAM snapshot: {phase}")
        if phase not in internal_heap:
            errors.append(f"memory: missing internal-heap snapshot: {phase}")

    arena_request = _ARENA_REQUEST_RE.search(text)
    if arena_request is None:
        errors.append("memory: missing PCM arena request")
    elif int(arena_request.group("bytes")) != expected_arena_bytes:
        errors.append(
            f"memory: arena request mismatch: expected {expected_arena_bytes}, "
            f"got {arena_request.group('bytes')}"
        )

    if storage.psram_bytes != 8 * 1024 * 1024:
        errors.append(f"memory: expected 8 MiB PSRAM, got {storage.psram_bytes}")
    if storage.residency_capacity != expected_arena_bytes:
        errors.append(
            f"memory: residency capacity mismatch: expected {expected_arena_bytes}, "
            f"got {storage.residency_capacity}"
        )
    if prepare.arena_bytes != expected_arena_bytes:
        errors.append(
            f"memory: Program Prepare arena mismatch: expected {expected_arena_bytes}, "
            f"got {prepare.arena_bytes}"
        )

    memory_match = _M6_MEMORY_RE.search(text)
    memory = None
    if memory_match is None:
        errors.append("memory: missing M6_QUAL_MEMORY line")
    else:
        memory = M6Memory(
            capacity_bytes=int(memory_match.group("capacity")),
            used_bytes=int(memory_match.group("used")),
            high_water_bytes=int(memory_match.group("high_water")),
            largest_free_span_bytes=int(memory_match.group("largest_free_span")),
            resident_samples=int(memory_match.group("resident_samples")),
            allocation_failures=int(memory_match.group("allocation_failures")),
            stale_reference_faults=int(memory_match.group("stale_reference_faults")),
        )
        if memory.capacity_bytes != expected_arena_bytes:
            errors.append(
                f"memory: M6 arena capacity mismatch: expected {expected_arena_bytes}, "
                f"got {memory.capacity_bytes}"
            )
        if memory.high_water_bytes > memory.capacity_bytes:
            errors.append("memory: arena high-water exceeds capacity")
        if memory.allocation_failures != 0:
            errors.append(
                f"memory: residency allocation failures={memory.allocation_failures}"
            )
        if memory.stale_reference_faults != 0:
            errors.append(
                f"memory: stale residency references={memory.stale_reference_faults}"
            )

    if prepare.final is not None and prepare.final.high_water_bytes > expected_arena_bytes:
        errors.append("prepare: residency high-water exceeds configured arena")

    guaranteed_candidate_voices = (
        _guaranteed_candidate(render_cases, block_budget_us)
        if block_budget_us is not None
        else 0
    )

    return QualificationReport(
        storage=storage,
        prepare=prepare,
        render_cases=tuple(render_cases),
        psram=psram,
        internal_heap=internal_heap,
        memory=memory,
        block_budget_us=block_budget_us,
        target_voices=target_voices,
        guaranteed_candidate_voices=guaranteed_candidate_voices,
        errors=tuple(errors),
    )


def render_markdown(
    report: QualificationReport,
    *,
    title: str = "ESP32-S3 N16R8 M6 Core Qualification",
) -> str:
    status = "CORE PASS" if report.passed else "FAIL"
    lines = [
        f"# {title}",
        "",
        f"Status: **{status}**",
        "",
        "> This report qualifies the physical N16R8 memory/storage/Program-Prepare/"
        "deterministic-Voice render core. Physical I2S output, underrun behavior and "
        "end-to-end real-bank audio remain separate release gates.",
        "",
        "## Core decision",
        "",
        f"- target Voice count: `{report.target_voices}`",
        f"- contiguous zero-miss candidate: `{report.guaranteed_candidate_voices}` voices",
        f"- 64-frame render budget: `{report.block_budget_us}` us",
        f"- PCM arena: `{report.memory.capacity_bytes if report.memory else None}` bytes",
    ]

    after_render = report.psram.get("after render benchmark")
    internal_after_render = report.internal_heap.get("after render benchmark")
    if after_render is not None:
        lines.extend(
            [
                f"- PSRAM free after render benchmark: `{after_render.free_bytes}` bytes",
                f"- PSRAM largest free block after render benchmark: "
                f"`{after_render.largest_free_block_bytes}` bytes",
            ]
        )
    if internal_after_render is not None:
        lines.extend(
            [
                f"- internal heap free after render benchmark: "
                f"`{internal_after_render.free_bytes}` bytes",
                f"- internal heap largest free block after render benchmark: "
                f"`{internal_after_render.largest_free_block_bytes}` bytes",
            ]
        )

    lines.extend(
        [
            "",
            "## Render timing",
            "",
            "| Voices | P95 us | P99 us | Max us | Deadline misses | Clipped | Realtime gate |",
            "|---:|---:|---:|---:|---:|---:|:---|",
        ]
    )
    budget = report.block_budget_us or DEFAULT_BLOCK_BUDGET_US
    for case in report.render_cases:
        gate = "PASS" if _case_meets_realtime_gate(case, budget) else "FAIL"
        lines.append(
            f"| {case.voices} | {case.p95_us} | {case.p99_us} | {case.max_us} | "
            f"{case.deadline_misses} | {case.clipped_samples} | {gate} |"
        )

    lines.extend(["", "## Program Prepare", ""])
    if report.prepare.cold is not None:
        lines.append(
            f"- cold initial: `{report.prepare.cold.elapsed_us}` us, "
            f"loaded `{report.prepare.cold.loaded_bytes}` bytes"
        )
    if report.prepare.warm is not None:
        lines.append(
            f"- warm same program: `{report.prepare.warm.elapsed_us}` us, "
            f"reused `{report.prepare.warm.reused_resident}` resident samples"
        )
    if report.prepare.eviction is not None:
        lines.append(
            f"- eviction Program Change: `{report.prepare.eviction.elapsed_us}` us, "
            f"evictions `{report.prepare.eviction.evictions}`, "
            f"evicted `{report.prepare.eviction.evicted_bytes}` bytes"
        )
    if report.prepare.final is not None:
        lines.append(
            f"- residency high-water: `{report.prepare.final.high_water_bytes}` bytes"
        )

    lines.extend(
        [
            "",
            "## Storage",
            "",
            "| Chunk | MiB/s | P95 us | P99 us | Max us |",
            "|---:|---:|---:|---:|---:|",
        ]
    )
    for chunk in report.storage.chunks:
        lines.append(
            f"| {chunk.chunk_size} | {chunk.throughput_mib_s:.3f} | "
            f"{chunk.p95_us} | {chunk.p99_us} | {chunk.max_us} |"
        )

    if report.errors:
        lines.extend(["", "## Failures", ""])
        lines.extend(f"- {error}" for error in report.errors)

    lines.extend(
        [
            "",
            "## Remaining release gates",
            "",
            "- Physical I2S output with zero unrecovered underruns.",
            "- End-to-end real-SF2 mixed 16-channel playback timing on N16R8.",
            "- Chorus/Reverb-enabled musical stress with the selected production audio path.",
            "- Long-run release/steal behavior on target hardware.",
            "",
        ]
    )
    return "\n".join(lines)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("log", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--title", default="ESP32-S3 N16R8 M6 Core Qualification")
    parser.add_argument("--expected-arena-bytes", type=int, default=DEFAULT_ARENA_BYTES)
    parser.add_argument("--expected-storage-bytes", type=int, default=DEFAULT_STORAGE_BYTES)
    parser.add_argument("--expected-samples", type=int, default=DEFAULT_SAMPLE_COUNT)
    parser.add_argument("--target-voices", type=int, default=DEFAULT_TARGET_VOICES)
    args = parser.parse_args()

    text = args.log.read_text(encoding="utf-8", errors="replace")
    report = parse_log(
        text,
        expected_arena_bytes=args.expected_arena_bytes,
        expected_storage_bytes=args.expected_storage_bytes,
        expected_samples=args.expected_samples,
        target_voices=args.target_voices,
    )
    markdown = render_markdown(report, title=args.title)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(markdown, encoding="utf-8")
    print(markdown)
    if not report.passed:
        sys.exit(1)


if __name__ == "__main__":
    main()
