from __future__ import annotations

import argparse
import json
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Iterable


@dataclass(frozen=True)
class AssetHeader:
    asset: str
    profile: str
    required_accept: int
    source_kind: str
    path: str
    file_bytes: int
    presets: int
    samples: int


@dataclass(frozen=True)
class ProgramRow:
    asset: str
    bank: int
    program: int
    program_class: str
    status: str
    reason: str
    channel: int
    native_zones: int
    zones: int
    deps: int
    stereo_pairs: int
    pcm_bytes: int
    aligned_pcm_bytes: int
    representative_overlap: int
    supported_modulators: int
    unsupported_modulators: int
    preview_zones: int
    coverage_misses: int
    coverage_required: int


@dataclass(frozen=True)
class AssetSummary:
    asset: str
    profile: str
    classification: str
    required_accept: int
    source_kind: str
    file_bytes: int
    presets: int
    rows: int
    banks: int
    bank0_programs: int
    bank128_programs: int
    missing_bank0: int
    missing_bank128: int
    metadata_failures: int
    duplicate_identities: int
    program_range_failures: int
    prepare_failures: int
    dependency_failures: int
    rejected_rows: int
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
    max_representative_overlap: int
    max_overlap_bank: int
    max_overlap_program: int


@dataclass
class AssetReport:
    header: AssetHeader | None
    programs: list[ProgramRow]
    summary: AssetSummary | None
    errors: list[str]

    @property
    def asset(self) -> str:
        if self.summary is not None:
            return self.summary.asset
        if self.header is not None:
            return self.header.asset
        return "unknown"

    @property
    def scanned(self) -> bool:
        return not self.errors and self.summary is not None

    @property
    def accepted(self) -> bool:
        return self.scanned and self.summary is not None and self.summary.classification == "ACCEPTED"

    @property
    def required_accept(self) -> bool:
        return bool(self.summary.required_accept) if self.summary is not None else False

    @property
    def passed(self) -> bool:
        return self.scanned and (not self.required_accept or self.accepted)


@dataclass
class PortfolioReport:
    assets: list[AssetReport]
    errors: list[str]
    min_assets: int

    @property
    def passed(self) -> bool:
        return not self.errors and len(self.assets) >= self.min_assets and all(
            asset.passed for asset in self.assets
        )


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


def _header(fields: dict[str, str]) -> AssetHeader:
    return AssetHeader(
        asset=fields["asset"],
        profile=fields["profile"],
        required_accept=_int(fields, "required_accept"),
        source_kind=fields["source_kind"],
        path=fields["path"],
        file_bytes=_int(fields, "file_bytes"),
        presets=_int(fields, "presets"),
        samples=_int(fields, "samples"),
    )


def _program(fields: dict[str, str]) -> ProgramRow:
    return ProgramRow(
        asset=fields["asset"],
        bank=_int(fields, "bank"),
        program=_int(fields, "program"),
        program_class=fields.get("class", "unknown"),
        status=fields.get("status", "UNKNOWN"),
        reason=fields.get("reason", "unknown"),
        channel=_int(fields, "channel"),
        native_zones=_int(fields, "native_zones"),
        zones=_int(fields, "zones"),
        deps=_int(fields, "deps"),
        stereo_pairs=_int(fields, "stereo_pairs"),
        pcm_bytes=_int(fields, "pcm_bytes"),
        aligned_pcm_bytes=_int(fields, "aligned_pcm_bytes"),
        representative_overlap=_int(fields, "representative_overlap"),
        supported_modulators=_int(fields, "supported_modulators"),
        unsupported_modulators=_int(fields, "unsupported_modulators"),
        preview_zones=_int(fields, "preview_zones"),
        coverage_misses=_int(fields, "coverage_misses"),
        coverage_required=_int(fields, "coverage_required"),
    )


def _summary(fields: dict[str, str]) -> AssetSummary:
    integer_names = (
        "required_accept",
        "file_bytes",
        "presets",
        "rows",
        "banks",
        "bank0_programs",
        "bank128_programs",
        "missing_bank0",
        "missing_bank128",
        "metadata_failures",
        "duplicate_identities",
        "program_range_failures",
        "prepare_failures",
        "dependency_failures",
        "rejected_rows",
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
        "max_representative_overlap",
        "max_overlap_bank",
        "max_overlap_program",
    )
    values = {name: _int(fields, name) for name in integer_names}
    return AssetSummary(
        asset=fields["asset"],
        profile=fields["profile"],
        classification=fields["classification"],
        source_kind=fields["source_kind"],
        **values,
    )


def parse_asset_log(text: str) -> AssetReport:
    headers: list[AssetHeader] = []
    programs: list[ProgramRow] = []
    summaries: list[AssetSummary] = []
    errors: list[str] = []

    for raw in text.splitlines():
        line = raw.strip()
        try:
            if line.startswith("M7_SF2_ASSET "):
                headers.append(_header(_parse_fields(line)))
            elif line.startswith("M7_SF2_PROGRAM "):
                programs.append(_program(_parse_fields(line)))
            elif line.startswith("M7_SF2_SUMMARY "):
                summaries.append(_summary(_parse_fields(line)))
            elif line.startswith("M7_SF2_FATAL"):
                errors.append(line)
        except (KeyError, TypeError, ValueError) as exc:
            errors.append(f"invalid M7 SF2 line: {exc}")

    if len(headers) != 1:
        errors.append(f"expected exactly one M7_SF2_ASSET, found {len(headers)}")
    if len(summaries) != 1:
        errors.append(f"expected exactly one M7_SF2_SUMMARY, found {len(summaries)}")
    header = headers[0] if len(headers) == 1 else None
    summary = summaries[0] if len(summaries) == 1 else None

    if header is not None and summary is not None:
        if header.asset != summary.asset:
            errors.append(f"asset mismatch header={header.asset} summary={summary.asset}")
        if header.profile != summary.profile:
            errors.append(f"profile mismatch asset={summary.asset}")
        if header.required_accept != summary.required_accept:
            errors.append(f"required_accept mismatch asset={summary.asset}")
        if header.source_kind != summary.source_kind:
            errors.append(f"source_kind mismatch asset={summary.asset}")
        if header.file_bytes != summary.file_bytes:
            errors.append(f"file_bytes mismatch asset={summary.asset}")
        if header.presets != summary.presets:
            errors.append(f"preset count mismatch asset={summary.asset}")

    if summary is not None:
        if summary.rows != len(programs):
            errors.append(
                f"asset={summary.asset} summary rows={summary.rows} parsed rows={len(programs)}"
            )
        rejected = sum(row.status != "PASS" for row in programs)
        if summary.rejected_rows != rejected:
            errors.append(
                f"asset={summary.asset} rejected_rows={summary.rejected_rows} parsed={rejected}"
            )
        for row in programs:
            if row.asset != summary.asset:
                errors.append(
                    f"program asset mismatch summary={summary.asset} row={row.asset}"
                )
        if summary.classification not in {"ACCEPTED", "REJECTED"}:
            errors.append(
                f"asset={summary.asset} invalid classification={summary.classification}"
            )
        if summary.classification == "ACCEPTED":
            blockers = (
                summary.metadata_failures
                + summary.duplicate_identities
                + summary.program_range_failures
                + summary.prepare_failures
                + summary.dependency_failures
                + summary.rejected_rows
            )
            if blockers != 0 or summary.rows != summary.presets:
                errors.append(f"asset={summary.asset} ACCEPTED with blocking counters")
            if summary.profile == "gm128" and (
                summary.missing_bank0 != 0 or summary.missing_bank128 != 0
            ):
                errors.append(f"asset={summary.asset} gm128 ACCEPTED with missing programs")
        if summary.required_accept and summary.classification != "ACCEPTED":
            errors.append(f"required asset rejected: {summary.asset}")

    return AssetReport(header=header, programs=programs, summary=summary, errors=errors)


def parse_portfolio(logs: Iterable[str], min_assets: int = 1) -> PortfolioReport:
    assets = [parse_asset_log(text) for text in logs]
    errors: list[str] = []
    identities: set[str] = set()
    for report in assets:
        errors.extend(report.errors)
        if report.asset in identities:
            errors.append(f"duplicate asset report: {report.asset}")
        identities.add(report.asset)
    if len(assets) < min_assets:
        errors.append(f"expected at least {min_assets} assets, found {len(assets)}")
    return PortfolioReport(assets=assets, errors=errors, min_assets=min_assets)


def _top(rows: Iterable[ProgramRow], field: str, limit: int = 8) -> list[ProgramRow]:
    return sorted(rows, key=lambda row: getattr(row, field), reverse=True)[:limit]


def render_markdown(
    portfolio: PortfolioReport,
    title: str = "M7 native-SF2 release portfolio",
) -> str:
    lines = [f"# {title}", "", f"Status: **{'PASS' if portfolio.passed else 'FAIL'}**", ""]
    lines.extend(
        [
            "## Asset classifications",
            "",
            "| Asset | Source | Profile | Required | Classification | Presets | Banks | Rejected rows | Max PCM | Max overlap |",
            "| --- | --- | --- | ---: | --- | ---: | ---: | ---: | ---: | ---: |",
        ]
    )
    for report in portfolio.assets:
        if report.summary is None:
            lines.append(f"| {report.asset} | ? | ? | ? | INVALID | ? | ? | ? | ? | ? |")
            continue
        summary = report.summary
        lines.append(
            f"| {summary.asset} | {summary.source_kind} | {summary.profile} | "
            f"{summary.required_accept} | {summary.classification} | {summary.presets} | "
            f"{summary.banks} | {summary.rejected_rows} | {summary.max_aligned_pcm_bytes} | "
            f"{summary.max_representative_overlap} |"
        )

    for report in portfolio.assets:
        if report.summary is None:
            continue
        summary = report.summary
        lines.extend(
            [
                "",
                f"## {summary.asset}",
                "",
                f"- classification: **{summary.classification}**",
                f"- source: `{summary.source_kind}` / `{report.header.path if report.header else 'unknown'}`",
                f"- programs scanned: `{summary.rows}/{summary.presets}` across `{summary.banks}` banks",
                f"- Bank 0 coverage: `{summary.bank0_programs}/128`",
                f"- Bank 128 coverage: `{summary.bank128_programs}/128`",
                f"- prepare failures: `{summary.prepare_failures}`",
                f"- unsupported modulators observed: `{summary.unsupported_modulators}`",
                f"- Preview zones observed: `{summary.preview_zones}`",
                f"- maximum playback zones: `{summary.max_zones}` at bank `{summary.max_zones_bank}` / program `{summary.max_zones_program}`",
                f"- maximum dependencies: `{summary.max_dependencies}` at bank `{summary.max_dependencies_bank}` / program `{summary.max_dependencies_program}`",
                f"- maximum aligned PCM working set: `{summary.max_aligned_pcm_bytes}` B at bank `{summary.max_pcm_bank}` / program `{summary.max_pcm_program}`",
                f"- maximum representative overlap: `{summary.max_representative_overlap}` at bank `{summary.max_overlap_bank}` / program `{summary.max_overlap_program}`",
            ]
        )
        rejected = [row for row in report.programs if row.status != "PASS"]
        if rejected:
            lines.extend(["", "Rejected program reasons:", ""])
            reason_counts: dict[str, int] = {}
            for row in rejected:
                reason_counts[row.reason] = reason_counts.get(row.reason, 0) + 1
            for reason, count in sorted(reason_counts.items()):
                lines.append(f"- `{reason}`: {count}")
        if report.programs:
            lines.extend(
                [
                    "",
                    "Largest PCM programs:",
                    "",
                    "| Bank | Program | Class | PCM aligned | Zones | Deps | Overlap | Status |",
                    "| ---: | ---: | --- | ---: | ---: | ---: | ---: | --- |",
                ]
            )
            for row in _top(report.programs, "aligned_pcm_bytes"):
                lines.append(
                    f"| {row.bank} | {row.program} | {row.program_class} | "
                    f"{row.aligned_pcm_bytes} | {row.zones} | {row.deps} | "
                    f"{row.representative_overlap} | {row.status} |"
                )

    if portfolio.errors:
        lines.extend(["", "## Report errors", ""])
        lines.extend(f"- {error}" for error in portfolio.errors)
    lines.append("")
    return "\n".join(lines)


def report_json(portfolio: PortfolioReport) -> dict[str, object]:
    accepted = sum(asset.accepted for asset in portfolio.assets)
    rejected = sum(asset.scanned and not asset.accepted for asset in portfolio.assets)
    return {
        "schema": "sf2-release-portfolio-v1",
        "passed": portfolio.passed,
        "min_assets": portfolio.min_assets,
        "asset_count": len(portfolio.assets),
        "accepted_assets": accepted,
        "rejected_assets": rejected,
        "errors": portfolio.errors,
        "assets": [
            {
                "asset": asset.asset,
                "scanned": asset.scanned,
                "accepted": asset.accepted,
                "passed": asset.passed,
                "errors": asset.errors,
                "header": asdict(asset.header) if asset.header is not None else None,
                "summary": asdict(asset.summary) if asset.summary is not None else None,
                "programs": [asdict(row) for row in asset.programs],
            }
            for asset in portfolio.assets
        ],
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Render the M7 native-SF2 release portfolio")
    parser.add_argument("--log", action="append", required=True, type=Path)
    parser.add_argument("--json", dest="json_path", required=True, type=Path)
    parser.add_argument("--markdown", required=True, type=Path)
    parser.add_argument("--min-assets", type=int, default=1)
    parser.add_argument("--title", default="M7 native-SF2 release portfolio")
    args = parser.parse_args(argv)

    portfolio = parse_portfolio(
        [path.read_text(encoding="utf-8", errors="replace") for path in args.log],
        min_assets=args.min_assets,
    )
    args.json_path.parent.mkdir(parents=True, exist_ok=True)
    args.markdown.parent.mkdir(parents=True, exist_ok=True)
    args.json_path.write_text(json.dumps(report_json(portfolio), indent=2) + "\n", encoding="utf-8")
    rendered = render_markdown(portfolio, title=args.title)
    args.markdown.write_text(rendered, encoding="utf-8")
    print(rendered)
    return 0 if portfolio.passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
