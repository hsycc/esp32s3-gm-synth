from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path
import re
import sys

_FAILURE_PATTERNS = (
    "Guru Meditation",
    "assert failed",
    "abort()",
    "panic'ed",
    "Program Prepare hardware benchmark FAILED",
)


@dataclass(frozen=True)
class PrepareCase:
    name: str
    elapsed_us: int
    dependencies: int
    dependency_pcm_bytes: int
    dependency_aligned_bytes: int
    loaded: int
    loaded_bytes: int
    reused_resident: int
    evictions: int
    evicted_bytes: int
    capacity_retries: int


@dataclass(frozen=True)
class CommitRetireCase:
    name: str
    commit_us: int
    retire_us: int
    release_tail_survived: bool | None = None
    release_ok: bool | None = None


@dataclass(frozen=True)
class PrimeCacheCase:
    elapsed_us: int
    loaded_samples: int
    loaded_bytes: int
    used_bytes: int
    largest_free_span: int
    free_spans: int


@dataclass(frozen=True)
class FinalState:
    used_bytes: int
    high_water_bytes: int
    largest_free_span: int
    resident_samples: int
    total_prepare_successes: int
    total_loaded_bytes: int
    total_evictions: int
    total_evicted_bytes: int


@dataclass(frozen=True)
class BenchmarkReport:
    source_path: str | None
    sample_count: int | None
    arena_bytes: int | None
    read_chunk_bytes: int | None
    cold: PrepareCase | None
    warm: PrepareCase | None
    prime: PrimeCacheCase | None
    eviction: PrepareCase | None
    warm_commit: CommitRetireCase | None
    eviction_commit: CommitRetireCase | None
    final: FinalState | None
    errors: tuple[str, ...]

    @property
    def passed(self) -> bool:
        return not self.errors


_START_RE = re.compile(
    r"prepare_bench: start path=(\S+) samples=(\d+) arena=(\d+) "
    r"A\[count=(\d+) aligned=(\d+)\] B\[count=(\d+) aligned=(\d+)\] read_chunk=(\d+)"
)
_PREPARE_RE = re.compile(
    r"prepare_bench: case=(cold_initial|warm_same_program|eviction_program_change) "
    r"ok=(\d+) elapsed_us=(\d+) dependencies=(\d+) dependency_pcm_bytes=(\d+) "
    r"dependency_aligned_bytes=(\d+) loaded=(\d+) loaded_bytes=(\d+) "
    r"reused_resident=(\d+) evictions=(\d+) evicted_bytes=(\d+) capacity_retries=(\d+)"
)
_PRIME_RE = re.compile(
    r"prepare_bench: case=prime_cache ok=(\d+) elapsed_us=(\d+) loaded_samples=(\d+) "
    r"loaded_bytes=(\d+) used=(\d+) largest_free_span=(\d+) free_spans=(\d+)"
)
_COMMIT_RE = re.compile(
    r"prepare_bench: case=(warm_commit_retire|eviction_commit_retire) "
    r"commit_ok=(\d+) ack_ok=(\d+) commit_us=(\d+) retire_us=(\d+)"
    r"(?: release_tail_survived=(\d+) release_ok=(\d+))?"
)
_FINAL_RE = re.compile(
    r"prepare_bench: PASS used=(\d+) high_water=(\d+) largest_free_span=(\d+) "
    r"resident_samples=(\d+) total_prepare_successes=(\d+) total_loaded_bytes=(\d+) "
    r"total_evictions=(\d+) total_evicted_bytes=(\d+)"
)


def _prepare_case(match: re.Match[str]) -> PrepareCase:
    return PrepareCase(
        name=match.group(1),
        elapsed_us=int(match.group(3)),
        dependencies=int(match.group(4)),
        dependency_pcm_bytes=int(match.group(5)),
        dependency_aligned_bytes=int(match.group(6)),
        loaded=int(match.group(7)),
        loaded_bytes=int(match.group(8)),
        reused_resident=int(match.group(9)),
        evictions=int(match.group(10)),
        evicted_bytes=int(match.group(11)),
        capacity_retries=int(match.group(12)),
    )


def parse_log(text: str, *, expected_samples: int | None = 67) -> BenchmarkReport:
    errors: list[str] = []
    for pattern in _FAILURE_PATTERNS:
        if pattern in text:
            errors.append(f"failure signature present: {pattern}")

    start = _START_RE.search(text)
    source_path = start.group(1) if start else None
    sample_count = int(start.group(2)) if start else None
    arena_bytes = int(start.group(3)) if start else None
    a_count = int(start.group(4)) if start else None
    b_count = int(start.group(6)) if start else None
    read_chunk_bytes = int(start.group(8)) if start else None

    prepare_matches = {match.group(1): match for match in _PREPARE_RE.finditer(text)}
    cold_match = prepare_matches.get("cold_initial")
    warm_match = prepare_matches.get("warm_same_program")
    eviction_match = prepare_matches.get("eviction_program_change")
    cold = _prepare_case(cold_match) if cold_match else None
    warm = _prepare_case(warm_match) if warm_match else None
    eviction = _prepare_case(eviction_match) if eviction_match else None

    prime_match = _PRIME_RE.search(text)
    prime = None
    if prime_match:
        if prime_match.group(1) != "1":
            errors.append("prime_cache reported ok=0")
        prime = PrimeCacheCase(
            elapsed_us=int(prime_match.group(2)),
            loaded_samples=int(prime_match.group(3)),
            loaded_bytes=int(prime_match.group(4)),
            used_bytes=int(prime_match.group(5)),
            largest_free_span=int(prime_match.group(6)),
            free_spans=int(prime_match.group(7)),
        )

    commit_matches = {match.group(1): match for match in _COMMIT_RE.finditer(text)}

    def parse_commit(name: str) -> CommitRetireCase | None:
        match = commit_matches.get(name)
        if match is None:
            return None
        if match.group(2) != "1" or match.group(3) != "1":
            errors.append(f"{name} reported commit/ack failure")
        return CommitRetireCase(
            name=name,
            commit_us=int(match.group(4)),
            retire_us=int(match.group(5)),
            release_tail_survived=(match.group(6) == "1") if match.group(6) is not None else None,
            release_ok=(match.group(7) == "1") if match.group(7) is not None else None,
        )

    warm_commit = parse_commit("warm_commit_retire")
    eviction_commit = parse_commit("eviction_commit_retire")

    final_match = _FINAL_RE.search(text)
    final = None
    if final_match:
        final = FinalState(
            used_bytes=int(final_match.group(1)),
            high_water_bytes=int(final_match.group(2)),
            largest_free_span=int(final_match.group(3)),
            resident_samples=int(final_match.group(4)),
            total_prepare_successes=int(final_match.group(5)),
            total_loaded_bytes=int(final_match.group(6)),
            total_evictions=int(final_match.group(7)),
            total_evicted_bytes=int(final_match.group(8)),
        )

    if start is None:
        errors.append("missing Program Prepare start line")
    if expected_samples is not None and sample_count != expected_samples:
        errors.append(f"sample-count mismatch: expected {expected_samples}, got {sample_count}")

    for name, match, case in (
        ("cold_initial", cold_match, cold),
        ("warm_same_program", warm_match, warm),
        ("eviction_program_change", eviction_match, eviction),
    ):
        if match is None or case is None:
            errors.append(f"missing prepare case: {name}")
        elif match.group(2) != "1":
            errors.append(f"{name} reported ok=0")

    if prime is None:
        errors.append("missing prime_cache case")
    if warm_commit is None:
        errors.append("missing warm_commit_retire case")
    if eviction_commit is None:
        errors.append("missing eviction_commit_retire case")
    if final is None:
        errors.append("missing Program Prepare PASS line")

    if cold is not None and (cold.loaded == 0 or cold.loaded_bytes == 0):
        errors.append("cold_initial did not load source-backed PCM")
    if warm is not None:
        if warm.loaded != 0:
            errors.append(f"warm_same_program unexpectedly loaded {warm.loaded} samples")
        if a_count is not None and warm.reused_resident != a_count:
            errors.append(
                f"warm_same_program reuse mismatch: expected {a_count}, got {warm.reused_resident}"
            )
    if eviction is not None:
        if eviction.loaded == 0:
            errors.append("eviction_program_change did not load source-backed PCM")
        if eviction.evictions == 0:
            errors.append("eviction_program_change did not exercise cache eviction")
        if b_count is not None and eviction.dependencies != b_count:
            errors.append(
                f"eviction dependency mismatch: expected {b_count}, got {eviction.dependencies}"
            )
    if eviction_commit is not None:
        if eviction_commit.release_tail_survived is not True:
            errors.append("release-tail Voice Ref did not survive retire")
        if eviction_commit.release_ok is not True:
            errors.append("release-tail Voice Ref could not be released")

    return BenchmarkReport(
        source_path=source_path,
        sample_count=sample_count,
        arena_bytes=arena_bytes,
        read_chunk_bytes=read_chunk_bytes,
        cold=cold,
        warm=warm,
        prime=prime,
        eviction=eviction,
        warm_commit=warm_commit,
        eviction_commit=eviction_commit,
        final=final,
        errors=tuple(errors),
    )


def render_markdown(report: BenchmarkReport, *, title: str = "Program Prepare Benchmark Report") -> str:
    status = "PASS" if report.passed else "FAIL"
    lines = [f"# {title}", "", f"Status: **{status}**", ""]
    lines.extend(
        [
            "## Configuration",
            "",
            f"- source path: `{report.source_path}`",
            f"- playable samples: `{report.sample_count}`",
            f"- PCM arena bytes: `{report.arena_bytes}`",
            f"- SF2 source read chunk: `{report.read_chunk_bytes}` bytes",
            "",
            "## Prepare cases",
            "",
            "| Case | Elapsed us | Dependencies | PCM bytes | Loaded | Loaded bytes | Reused resident | Evictions | Evicted bytes | Capacity retries |",
            "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|",
        ]
    )
    for case in (report.cold, report.warm, report.eviction):
        if case is None:
            continue
        lines.append(
            f"| {case.name} | {case.elapsed_us} | {case.dependencies} | "
            f"{case.dependency_pcm_bytes} | {case.loaded} | {case.loaded_bytes} | "
            f"{case.reused_resident} | {case.evictions} | {case.evicted_bytes} | "
            f"{case.capacity_retries} |"
        )

    if report.prime is not None:
        lines.extend(
            [
                "",
                "## Cache pressure",
                "",
                f"- prime elapsed: `{report.prime.elapsed_us}` us",
                f"- cache samples loaded: `{report.prime.loaded_samples}`",
                f"- cache PCM bytes loaded: `{report.prime.loaded_bytes}`",
                f"- residency used after prime: `{report.prime.used_bytes}` bytes",
                f"- largest free span after prime: `{report.prime.largest_free_span}` bytes",
                f"- free spans after prime: `{report.prime.free_spans}`",
            ]
        )

    lines.extend(
        [
            "",
            "## Commit / retire",
            "",
            "| Case | Commit us | Retire us | Release tail survived | Release ok |",
            "|---|---:|---:|---|---|",
        ]
    )
    for case in (report.warm_commit, report.eviction_commit):
        if case is None:
            continue
        tail = "n/a" if case.release_tail_survived is None else str(case.release_tail_survived)
        release = "n/a" if case.release_ok is None else str(case.release_ok)
        lines.append(f"| {case.name} | {case.commit_us} | {case.retire_us} | {tail} | {release} |")

    if report.final is not None:
        lines.extend(
            [
                "",
                "## Final residency / prepare state",
                "",
                f"- used bytes: `{report.final.used_bytes}`",
                f"- high-water bytes: `{report.final.high_water_bytes}`",
                f"- largest free span: `{report.final.largest_free_span}`",
                f"- resident samples: `{report.final.resident_samples}`",
                f"- prepare successes: `{report.final.total_prepare_successes}`",
                f"- source bytes loaded: `{report.final.total_loaded_bytes}`",
                f"- cache evictions: `{report.final.total_evictions}`",
                f"- cache bytes evicted: `{report.final.total_evicted_bytes}`",
            ]
        )

    if report.errors:
        lines.extend(["", "## Validation errors", ""])
        lines.extend(f"- {error}" for error in report.errors)

    return "\n".join(lines) + "\n"


def _build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Parse ESP32-S3 Program Prepare benchmark serial output and emit Markdown."
    )
    parser.add_argument("log", type=Path, help="serial/QEMU log file")
    parser.add_argument("--expected-samples", type=int, default=67)
    parser.add_argument("--title", default="Program Prepare Benchmark Report")
    parser.add_argument("--output", type=Path, default=None)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _build_arg_parser().parse_args(argv)
    text = args.log.read_text(encoding="utf-8", errors="replace")
    report = parse_log(text, expected_samples=args.expected_samples)
    markdown = render_markdown(report, title=args.title)
    if args.output is None:
        sys.stdout.write(markdown)
    else:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(markdown, encoding="utf-8")
    return 0 if report.passed else 2


if __name__ == "__main__":
    raise SystemExit(main())
