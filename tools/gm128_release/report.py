from __future__ import annotations

import argparse
import json
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Iterable


@dataclass(frozen=True)
class ProgramRow:
    bank: int
    program: int
    program_class: str
    status: str
    channel: int = 0
    native_zones: int = 0
    zones: int = 0
    deps: int = 0
    stereo_pairs: int = 0
    pcm_bytes: int = 0
    aligned_pcm_bytes: int = 0
    supported_modulators: int = 0
    unsupported_modulators: int = 0
    preview_zones: int = 0
    coverage_misses: int = 0
    drum_checked: int = 0


@dataclass(frozen=True)
class MatrixSummary:
    rows: int
    melodic_programs: int
    percussion_programs: int
    missing_melodic: int
    missing_percussion: int
    prepare_failures: int
    coverage_failures: int
    unsupported_modulators: int
    preview_zones: int
    max_zones: int
    max_zones_bank: int
    max_zones_program: int
    max_dependencies: int
    max_dependencies_bank: int
    max_dependencies_program: int
    max_aligned_pcm_bytes: int
    max_pcm_bank: int
    max_pcm_program: int


@dataclass
class ReleaseReport:
    programs: list[ProgramRow]
    summary: MatrixSummary | None
    errors: list[str]

    @property
    def passed(self) -> bool:
        return not self.errors


def _parse_fields(line: str) -> dict[str, str]:
    fields: dict[str, str] = {}
    for token in line.split()[1:]:
        if "=" not in token:
            continue
        key, value = token.split("=", 1)
        fields[key] = value
    return fields


def _int(fields: dict[str, str], name: str, default: int = 0) -> int:
    value = fields.get(name)
    if value is None:
        return default
    return int(value, 0)


def _program_row(fields: dict[str, str]) -> ProgramRow:
    return ProgramRow(
        bank=_int(fields, "bank"),
        program=_int(fields, "program"),
        program_class=fields.get("class", "unknown"),
        status=fields.get("status", "UNKNOWN"),
        channel=_int(fields, "channel"),
        native_zones=_int(fields, "native_zones"),
        zones=_int(fields, "zones"),
        deps=_int(fields, "deps"),
        stereo_pairs=_int(fields, "stereo_pairs"),
        pcm_bytes=_int(fields, "pcm_bytes"),
        aligned_pcm_bytes=_int(fields, "aligned_pcm_bytes"),
        supported_modulators=_int(fields, "supported_modulators"),
        unsupported_modulators=_int(fields, "unsupported_modulators"),
        preview_zones=_int(fields, "preview_zones"),
        coverage_misses=_int(fields, "coverage_misses"),
        drum_checked=_int(fields, "drum_checked"),
    )


def _matrix_summary(fields: dict[str, str]) -> MatrixSummary:
    names = (
        "rows",
        "melodic_programs",
        "percussion_programs",
        "missing_melodic",
        "missing_percussion",
        "prepare_failures",
        "coverage_failures",
        "unsupported_modulators",
        "preview_zones",
        "max_zones",
        "max_zones_bank",
        "max_zones_program",
        "max_dependencies",
        "max_dependencies_bank",
        "max_dependencies_program",
        "max_aligned_pcm_bytes",
        "max_pcm_bank",
        "max_pcm_program",
    )
    return MatrixSummary(**{name: _int(fields, name) for name in names})


def parse_log(text: str) -> ReleaseReport:
    programs: list[ProgramRow] = []
    summaries: list[MatrixSummary] = []
    errors: list[str] = []

    for raw in text.splitlines():
        line = raw.strip()
        if line.startswith("M7_PROGRAM "):
            try:
                programs.append(_program_row(_parse_fields(line)))
            except (TypeError, ValueError) as exc:
                errors.append(f"invalid M7_PROGRAM line: {exc}")
        elif line.startswith("M7_MATRIX_SUMMARY "):
            try:
                summaries.append(_matrix_summary(_parse_fields(line)))
            except (TypeError, ValueError) as exc:
                errors.append(f"invalid M7_MATRIX_SUMMARY line: {exc}")
        elif line.startswith("M7_MATRIX_FAILURE"):
            errors.append(line)

    if len(summaries) != 1:
        errors.append(f"expected exactly one M7_MATRIX_SUMMARY, found {len(summaries)}")
    summary = summaries[0] if len(summaries) == 1 else None

    identities: set[tuple[int, int]] = set()
    for row in programs:
        identity = (row.bank, row.program)
        if identity in identities:
            errors.append(f"duplicate program row bank={row.bank} program={row.program}")
        identities.add(identity)
        if row.status != "PASS":
            errors.append(f"program failed bank={row.bank} program={row.program}")
        if row.unsupported_modulators != 0:
            errors.append(
                f"unsupported modulators bank={row.bank} program={row.program}: "
                f"{row.unsupported_modulators}"
            )
        if row.preview_zones != 0:
            errors.append(f"preview zones bank={row.bank} program={row.program}: {row.preview_zones}")
        if row.coverage_misses != 0:
            errors.append(f"coverage misses bank={row.bank} program={row.program}: {row.coverage_misses}")

    melodic = {row.program for row in programs if row.bank == 0 and row.program_class == "melodic"}
    percussion = {
        row.program for row in programs if row.bank == 128 and row.program_class == "percussion"
    }
    expected = set(range(128))
    missing_melodic = sorted(expected - melodic)
    if missing_melodic:
        errors.append(f"missing melodic programs: {missing_melodic}")
    if 0 not in percussion:
        errors.append("missing standard percussion program bank=128 program=0")

    if summary is not None:
        if summary.rows != len(programs):
            errors.append(f"summary rows={summary.rows} but parsed rows={len(programs)}")
        if summary.melodic_programs != len(melodic):
            errors.append(
                f"summary melodic_programs={summary.melodic_programs} but parsed={len(melodic)}"
            )
        if summary.percussion_programs != len(percussion):
            errors.append(
                "summary percussion_programs="
                f"{summary.percussion_programs} but parsed={len(percussion)}"
            )
        if summary.missing_melodic != 0:
            errors.append(f"summary missing_melodic={summary.missing_melodic}")
        if summary.prepare_failures != 0:
            errors.append(f"summary prepare_failures={summary.prepare_failures}")
        if summary.coverage_failures != 0:
            errors.append(f"summary coverage_failures={summary.coverage_failures}")
        if summary.unsupported_modulators != 0:
            errors.append(f"summary unsupported_modulators={summary.unsupported_modulators}")
        if summary.preview_zones != 0:
            errors.append(f"summary preview_zones={summary.preview_zones}")

    return ReleaseReport(programs=programs, summary=summary, errors=errors)


def _top(rows: Iterable[ProgramRow], field: str, limit: int = 8) -> list[ProgramRow]:
    return sorted(rows, key=lambda row: getattr(row, field), reverse=True)[:limit]


def render_markdown(report: ReleaseReport, title: str = "M7 GM128 release matrix") -> str:
    lines = [f"# {title}", ""]
    lines.append(f"Status: **{'PASS' if report.passed else 'FAIL'}**")
    lines.append("")

    melodic = [row for row in report.programs if row.program_class == "melodic"]
    percussion = [row for row in report.programs if row.program_class == "percussion"]
    lines.extend(
        [
            "## Coverage",
            "",
            f"- program rows: `{len(report.programs)}`",
            f"- melodic Bank 0 programs: `{len(melodic)}/128`",
            f"- percussion Bank 128 programs: `{len(percussion)}`",
        ]
    )
    if report.summary is not None:
        summary = report.summary
        lines.extend(
            [
                f"- prepare failures: `{summary.prepare_failures}`",
                f"- coverage failures: `{summary.coverage_failures}`",
                f"- unsupported modulators: `{summary.unsupported_modulators}`",
                f"- Preview zones: `{summary.preview_zones}`",
                "",
                "## Matrix maxima",
                "",
                "| Metric | Value | Program |",
                "| --- | ---: | --- |",
                f"| Playback zones | {summary.max_zones} | bank {summary.max_zones_bank} / program {summary.max_zones_program} |",
                f"| Sample dependencies | {summary.max_dependencies} | bank {summary.max_dependencies_bank} / program {summary.max_dependencies_program} |",
                f"| Aligned PCM working set | {summary.max_aligned_pcm_bytes} B | bank {summary.max_pcm_bank} / program {summary.max_pcm_program} |",
            ]
        )

    if report.programs:
        lines.extend(["", "## Largest PCM programs", "", "| Bank | Program | Class | Aligned PCM | Zones | Dependencies |", "| ---: | ---: | --- | ---: | ---: | ---: |"]) 
        for row in _top(report.programs, "aligned_pcm_bytes"):
            lines.append(
                f"| {row.bank} | {row.program} | {row.program_class} | "
                f"{row.aligned_pcm_bytes} | {row.zones} | {row.deps} |"
            )

    if report.errors:
        lines.extend(["", "## Errors", ""])
        lines.extend(f"- {error}" for error in report.errors)

    lines.append("")
    return "\n".join(lines)


def report_json(report: ReleaseReport) -> dict[str, object]:
    return {
        "schema": "gm128-release-matrix-v1",
        "passed": report.passed,
        "errors": report.errors,
        "summary": asdict(report.summary) if report.summary is not None else None,
        "programs": [asdict(row) for row in report.programs],
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Render the M7 GM128 release program matrix")
    parser.add_argument("--log", required=True, type=Path)
    parser.add_argument("--json", dest="json_path", required=True, type=Path)
    parser.add_argument("--markdown", required=True, type=Path)
    args = parser.parse_args(argv)

    report = parse_log(args.log.read_text(encoding="utf-8", errors="replace"))
    args.json_path.parent.mkdir(parents=True, exist_ok=True)
    args.markdown.parent.mkdir(parents=True, exist_ok=True)
    args.json_path.write_text(json.dumps(report_json(report), indent=2) + "\n", encoding="utf-8")
    args.markdown.write_text(render_markdown(report), encoding="utf-8")
    print(render_markdown(report))
    return 0 if report.passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
