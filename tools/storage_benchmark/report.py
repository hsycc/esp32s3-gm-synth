from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path
import re
import sys
from typing import Iterable

STANDARD_CHUNK_SIZES = (4096, 8192, 16384, 32768, 65536)
_FAILURE_PATTERNS = (
    "Guru Meditation",
    "assert failed",
    "abort()",
    "panic'ed",
    "Storage benchmark SF2 parse failed",
    "One or more storage benchmark chunk-size passes failed",
)


@dataclass(frozen=True)
class ChunkResult:
    chunk_size: int
    bytes_read: int
    chunk_count: int
    elapsed_us: int
    throughput_mib_s: float
    min_us: int
    avg_us: int
    p50_us: int
    p95_us: int
    p99_us: int
    max_us: int


@dataclass(frozen=True)
class BenchmarkReport:
    psram_bytes: int | None
    source_path: str | None
    file_bytes: int | None
    playable_samples: int | None
    smpl_offset: int | None
    smpl_bytes: int | None
    benchmark_bytes: int | None
    residency_capacity: int | None
    residency_used: int | None
    chunks: tuple[ChunkResult, ...]
    errors: tuple[str, ...]

    @property
    def passed(self) -> bool:
        return not self.errors


_PSRAM_RE = re.compile(r"Detected physical PSRAM:\s+(\d+) bytes")
_OPEN_RE = re.compile(r"Storage benchmark VFS open: path=(\S+) file_bytes=(\d+)")
_PARSED_RE = re.compile(r"Storage benchmark SF2 parsed: playable_samples=(\d+)")
_SOURCE_RE = re.compile(
    r"Storage benchmark source: path=(\S+) smpl_offset=(\d+) smpl_bytes=(\d+) benchmark_bytes=(\d+)"
)
_RESIDENCY_RE = re.compile(r"Residency ready: capacity=(\d+) used=(\d+)")
_CHUNK_RE = re.compile(
    r"chunk=(\d+) bytes=(\d+) chunks=(\d+) elapsed=(\d+) us "
    r"throughput=([0-9]+(?:\.[0-9]+)?) MiB/s "
    r"latency_us\[min/avg/p50/p95/p99/max\]=(\d+)/(\d+)/(\d+)/(\d+)/(\d+)/(\d+)"
)


def _first_match(pattern: re.Pattern[str], text: str) -> re.Match[str] | None:
    return pattern.search(text)


def parse_log(
    text: str,
    *,
    expected_bytes: int | None = None,
    expected_samples: int | None = None,
    expected_chunks: Iterable[int] = STANDARD_CHUNK_SIZES,
) -> BenchmarkReport:
    errors: list[str] = []

    for pattern in _FAILURE_PATTERNS:
        if pattern in text:
            errors.append(f"failure signature present: {pattern}")

    psram_match = _first_match(_PSRAM_RE, text)
    open_match = _first_match(_OPEN_RE, text)
    parsed_match = _first_match(_PARSED_RE, text)
    source_match = _first_match(_SOURCE_RE, text)
    residency_match = _first_match(_RESIDENCY_RE, text)

    psram_bytes = int(psram_match.group(1)) if psram_match else None
    source_path = None
    file_bytes = None
    if open_match:
        source_path = open_match.group(1)
        file_bytes = int(open_match.group(2))

    playable_samples = int(parsed_match.group(1)) if parsed_match else None

    smpl_offset = None
    smpl_bytes = None
    benchmark_bytes = None
    if source_match:
        source_path = source_match.group(1)
        smpl_offset = int(source_match.group(2))
        smpl_bytes = int(source_match.group(3))
        benchmark_bytes = int(source_match.group(4))

    residency_capacity = None
    residency_used = None
    if residency_match:
        residency_capacity = int(residency_match.group(1))
        residency_used = int(residency_match.group(2))

    chunk_results: dict[int, ChunkResult] = {}
    for match in _CHUNK_RE.finditer(text):
        chunk_size = int(match.group(1))
        chunk_results[chunk_size] = ChunkResult(
            chunk_size=chunk_size,
            bytes_read=int(match.group(2)),
            chunk_count=int(match.group(3)),
            elapsed_us=int(match.group(4)),
            throughput_mib_s=float(match.group(5)),
            min_us=int(match.group(6)),
            avg_us=int(match.group(7)),
            p50_us=int(match.group(8)),
            p95_us=int(match.group(9)),
            p99_us=int(match.group(10)),
            max_us=int(match.group(11)),
        )

    expected_chunk_tuple = tuple(expected_chunks)
    for chunk_size in expected_chunk_tuple:
        result = chunk_results.get(chunk_size)
        if result is None:
            errors.append(f"missing chunk result: {chunk_size}")
            continue
        if expected_bytes is not None and result.bytes_read != expected_bytes:
            errors.append(
                f"chunk {chunk_size} bytes mismatch: expected {expected_bytes}, got {result.bytes_read}"
            )

    if psram_bytes is None:
        errors.append("missing PSRAM detection line")
    if open_match is None:
        errors.append("missing VFS open line")
    if playable_samples is None:
        errors.append("missing SF2 parsed line")
    if source_match is None:
        errors.append("missing benchmark source line")
    if residency_match is None:
        errors.append("missing residency line")

    if expected_samples is not None and playable_samples != expected_samples:
        errors.append(
            f"playable sample mismatch: expected {expected_samples}, got {playable_samples}"
        )
    if expected_bytes is not None and benchmark_bytes != expected_bytes:
        errors.append(
            f"benchmark byte mismatch: expected {expected_bytes}, got {benchmark_bytes}"
        )

    ordered_chunks = tuple(
        chunk_results[size] for size in expected_chunk_tuple if size in chunk_results
    )

    return BenchmarkReport(
        psram_bytes=psram_bytes,
        source_path=source_path,
        file_bytes=file_bytes,
        playable_samples=playable_samples,
        smpl_offset=smpl_offset,
        smpl_bytes=smpl_bytes,
        benchmark_bytes=benchmark_bytes,
        residency_capacity=residency_capacity,
        residency_used=residency_used,
        chunks=ordered_chunks,
        errors=tuple(errors),
    )


def render_markdown(report: BenchmarkReport, *, title: str = "Storage Benchmark Report") -> str:
    status = "PASS" if report.passed else "FAIL"
    lines = [f"# {title}", "", f"Status: **{status}**", ""]

    lines.extend(
        [
            "## Source",
            "",
            f"- PSRAM bytes: `{report.psram_bytes}`",
            f"- source path: `{report.source_path}`",
            f"- file bytes: `{report.file_bytes}`",
            f"- playable samples: `{report.playable_samples}`",
            f"- smpl offset: `{report.smpl_offset}`",
            f"- smpl bytes: `{report.smpl_bytes}`",
            f"- benchmark bytes/pass: `{report.benchmark_bytes}`",
            f"- residency capacity: `{report.residency_capacity}`",
            f"- residency used: `{report.residency_used}`",
            "",
            "## Chunk sweep",
            "",
            "| Chunk | Bytes | Reads | Elapsed us | MiB/s | Min us | Avg us | P50 us | P95 us | P99 us | Max us |",
            "|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|",
        ]
    )

    for row in report.chunks:
        lines.append(
            "| "
            f"{row.chunk_size} | {row.bytes_read} | {row.chunk_count} | {row.elapsed_us} | "
            f"{row.throughput_mib_s:.3f} | {row.min_us} | {row.avg_us} | {row.p50_us} | "
            f"{row.p95_us} | {row.p99_us} | {row.max_us} |"
        )

    if report.errors:
        lines.extend(["", "## Validation errors", ""])
        lines.extend(f"- {error}" for error in report.errors)

    return "\n".join(lines) + "\n"


def _build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Parse ESP32-S3 storage benchmark serial output and emit Markdown."
    )
    parser.add_argument("log", type=Path, help="serial/QEMU log file")
    parser.add_argument("--expected-bytes", type=int, default=None)
    parser.add_argument("--expected-samples", type=int, default=None)
    parser.add_argument("--title", default="Storage Benchmark Report")
    parser.add_argument("--output", type=Path, default=None)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _build_arg_parser().parse_args(argv)
    text = args.log.read_text(encoding="utf-8", errors="replace")
    report = parse_log(
        text,
        expected_bytes=args.expected_bytes,
        expected_samples=args.expected_samples,
    )
    markdown = render_markdown(report, title=args.title)

    if args.output is None:
        sys.stdout.write(markdown)
    else:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(markdown, encoding="utf-8")

    return 0 if report.passed else 2


if __name__ == "__main__":
    raise SystemExit(main())
