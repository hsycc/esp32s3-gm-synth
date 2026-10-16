"""Render deterministic QEMU Voice DSP telemetry as Markdown."""

from __future__ import annotations

import argparse
import re
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

_CASE_RE = re.compile(
    r"render_bench: case "
    r"voices=(?P<voices>\d+) "
    r"blocks=(?P<blocks>\d+) "
    r"min_us=(?P<min_us>\d+) "
    r"avg_us=(?P<avg_us>\d+) "
    r"p50_us=(?P<p50_us>\d+) "
    r"p95_us=(?P<p95_us>\d+) "
    r"p99_us=(?P<p99_us>\d+) "
    r"max_us=(?P<max_us>\d+) "
    r"deadline_misses=(?P<deadline_misses>\d+) "
    r"clipped_samples=(?P<clipped_samples>\d+) "
    r"checksum=(?P<checksum>[0-9a-fA-F]+)"
)


@dataclass(frozen=True)
class RenderCase:
    voices: int
    blocks: int
    min_us: int
    avg_us: int
    p50_us: int
    p95_us: int
    p99_us: int
    max_us: int
    deadline_misses: int
    clipped_samples: int
    checksum: str


def parse_cases(text: str) -> list[RenderCase]:
    cases: list[RenderCase] = []
    for match in _CASE_RE.finditer(text):
        values = match.groupdict()
        cases.append(
            RenderCase(
                voices=int(values["voices"]),
                blocks=int(values["blocks"]),
                min_us=int(values["min_us"]),
                avg_us=int(values["avg_us"]),
                p50_us=int(values["p50_us"]),
                p95_us=int(values["p95_us"]),
                p99_us=int(values["p99_us"]),
                max_us=int(values["max_us"]),
                deadline_misses=int(values["deadline_misses"]),
                clipped_samples=int(values["clipped_samples"]),
                checksum=values["checksum"].lower(),
            )
        )
    return cases


def validate_cases(
    text: str,
    cases: Iterable[RenderCase],
    expected_voices: Iterable[int],
) -> list[RenderCase]:
    parsed = list(cases)
    expected = list(expected_voices)
    actual = [case.voices for case in parsed]
    if actual != expected:
        raise ValueError(f"render benchmark voices mismatch: expected={expected} actual={actual}")
    if "render_bench: PASS" not in text:
        raise ValueError("render benchmark PASS marker missing")
    if any(case.blocks <= 0 for case in parsed):
        raise ValueError("render benchmark contains an empty case")
    if any(case.p50_us > case.p95_us or case.p95_us > case.p99_us for case in parsed):
        raise ValueError("render benchmark percentiles are not monotonic")
    if any(case.min_us > case.p50_us or case.p99_us > case.max_us for case in parsed):
        raise ValueError("render benchmark min/max bounds are invalid")
    return parsed


def render_markdown(cases: Iterable[RenderCase], title: str) -> str:
    rows = list(cases)
    lines = [
        f"# {title}",
        "",
        "> QEMU timing is regression telemetry only. Do not use these values as ESP32-S3 N16R8 CPU, I2S deadline, or maximum-polyphony qualification data.",
        "",
        "| Voices | Blocks | Min µs | Avg µs | P50 µs | P95 µs | P99 µs | Max µs | Deadline misses | Clipped samples | Checksum |",
        "|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|:---|",
    ]
    for case in rows:
        lines.append(
            "| "
            f"{case.voices} | {case.blocks} | {case.min_us} | {case.avg_us} | "
            f"{case.p50_us} | {case.p95_us} | {case.p99_us} | {case.max_us} | "
            f"{case.deadline_misses} | {case.clipped_samples} | `{case.checksum}` |"
        )
    lines.extend(
        [
            "",
            "The workload uses a deterministic in-memory looping PCM fixture and fixed Voice counts, so changes between CI runs are useful for spotting render-path regressions. Physical-board timing remains the performance oracle.",
            "",
        ]
    )
    return "\n".join(lines)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("log", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--title", default="ESP32-S3 QEMU Render Timing Telemetry")
    parser.add_argument("--expected-voices", nargs="+", type=int, required=True)
    args = parser.parse_args()

    text = args.log.read_text(encoding="utf-8", errors="replace")
    cases = validate_cases(text, parse_cases(text), args.expected_voices)
    markdown = render_markdown(cases, args.title)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(markdown, encoding="utf-8")


if __name__ == "__main__":
    main()
