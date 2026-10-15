from __future__ import annotations

import argparse
import json
import re
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


@dataclass(frozen=True)
class ProgramFixture:
    bank: int
    program: int
    name: str
    sample_ids: tuple[int, ...]


@dataclass(frozen=True)
class BankFixture:
    name: str
    source_path: str
    programs: tuple[ProgramFixture, ...]
    unique_sample_ids: tuple[int, ...]
    aligned_pcm_bytes: int
    all_presets_aligned_pcm_bytes: int


def _program_key(item: dict) -> tuple[int, int]:
    return int(item["bank"]), int(item["program"])


def load_bank_fixture(path: Path) -> BankFixture:
    data = json.loads(path.read_text(encoding="utf-8"))
    working_sets = data.get("working_sets", {})
    greedy = working_sets.get("greedy_16_program_union")
    if not greedy:
        raise ValueError(f"{path}: missing greedy_16_program_union")

    programs_by_key = {_program_key(item): item for item in data.get("programs", [])}
    selected: list[ProgramFixture] = []
    for summary in greedy.get("programs", []):
        key = _program_key(summary)
        source = programs_by_key.get(key)
        if source is None:
            raise ValueError(f"{path}: working-set program {key} missing from programs[]")
        sample_ids = tuple(int(value) for value in source.get("sample_ids", []))
        if len(sample_ids) != len(set(sample_ids)):
            raise ValueError(f"{path}: program {key} contains duplicate SampleIds")
        selected.append(
            ProgramFixture(
                bank=key[0],
                program=key[1],
                name=str(source.get("name", summary.get("name", ""))),
                sample_ids=sample_ids,
            )
        )

    if not selected or len(selected) > 16:
        raise ValueError(f"{path}: expected 1..16 greedy programs, got {len(selected)}")

    union = tuple(sorted({sample_id for program in selected for sample_id in program.sample_ids}))
    declared_union = tuple(sorted(int(value) for value in greedy.get("sample_ids", [])))
    if union != declared_union:
        raise ValueError(f"{path}: greedy program dependency union does not match sample_ids")

    samples_by_id = {int(item["sample_id"]): item for item in data.get("samples", {}).get("items", [])}
    missing = [sample_id for sample_id in union if sample_id not in samples_by_id]
    if missing:
        raise ValueError(f"{path}: working-set SampleIds missing from samples[]: {missing[:8]}")

    if any(sample_id < 0 or sample_id >= 512 for sample_id in union):
        raise ValueError(f"{path}: SampleId outside firmware directory capacity 0..511")

    computed_aligned = sum(int(samples_by_id[sample_id]["aligned_pcm_bytes"]) for sample_id in union)
    declared_aligned = int(greedy["aligned_pcm_bytes"])
    if computed_aligned != declared_aligned:
        raise ValueError(
            f"{path}: aligned bytes mismatch computed={computed_aligned} declared={declared_aligned}"
        )

    all_presets = working_sets.get("all_presets_union")
    if not all_presets:
        raise ValueError(f"{path}: missing all_presets_union")

    return BankFixture(
        name=path.stem.removesuffix(".analysis"),
        source_path=path.as_posix(),
        programs=tuple(selected),
        unique_sample_ids=union,
        aligned_pcm_bytes=declared_aligned,
        all_presets_aligned_pcm_bytes=int(all_presets["aligned_pcm_bytes"]),
    )


def _identifier(value: str) -> str:
    value = re.sub(r"[^A-Za-z0-9]+", "_", value).strip("_")
    if not value or value[0].isdigit():
        value = f"bank_{value}"
    return value


def _cpp_ids(values: Iterable[int]) -> str:
    return ", ".join(f"{value}u" for value in values)


def render_cpp_header(fixtures: list[BankFixture]) -> str:
    lines = [
        "// Generated from checked-in SF2 analyzer manifests. DO NOT EDIT.",
        "#pragma once",
        "",
        "#include <cstddef>",
        "#include <cstdint>",
        "",
        "namespace gm_synth::test_fixture {",
        "",
        "struct ProgramDependencyFixture {",
        "    std::uint16_t bank;",
        "    std::uint8_t program;",
        "    const char* name;",
        "    const std::uint32_t* sample_ids;",
        "    std::size_t sample_count;",
        "};",
        "",
        "struct BankDependencyFixture {",
        "    const char* name;",
        "    const char* source_manifest;",
        "    const ProgramDependencyFixture* programs;",
        "    std::size_t program_count;",
        "    const std::uint32_t* unique_sample_ids;",
        "    std::size_t unique_sample_count;",
        "    std::size_t greedy_aligned_pcm_bytes;",
        "    std::size_t all_presets_aligned_pcm_bytes;",
        "};",
        "",
    ]

    bank_names: list[str] = []
    for bank_index, fixture in enumerate(fixtures):
        prefix = f"kBank{bank_index}_{_identifier(fixture.name)}"
        bank_names.append(prefix)
        for program_index, program in enumerate(fixture.programs):
            ids_name = f"{prefix}_Program{program_index}_Samples"
            lines.append(
                f"inline constexpr std::uint32_t {ids_name}[] = {{{_cpp_ids(program.sample_ids)}}};"
            )
        lines.append("")
        lines.append(f"inline constexpr ProgramDependencyFixture {prefix}_Programs[] = {{")
        for program_index, program in enumerate(fixture.programs):
            ids_name = f"{prefix}_Program{program_index}_Samples"
            escaped_name = program.name.replace("\\", "\\\\").replace('"', '\\"')
            lines.append(
                "    {"
                f"{program.bank}u, {program.program}u, \"{escaped_name}\", "
                f"{ids_name}, sizeof({ids_name}) / sizeof({ids_name}[0])"
                "},"
            )
        lines.append("};")
        lines.append(
            f"inline constexpr std::uint32_t {prefix}_UniqueSamples[] = "
            f"{{{_cpp_ids(fixture.unique_sample_ids)}}};"
        )
        lines.append("")

    lines.append("inline constexpr BankDependencyFixture kRealBankFixtures[] = {")
    for prefix, fixture in zip(bank_names, fixtures):
        name = fixture.name.replace("\\", "\\\\").replace('"', '\\"')
        source = fixture.source_path.replace("\\", "\\\\").replace('"', '\\"')
        lines.append(
            "    {"
            f"\"{name}\", \"{source}\", "
            f"{prefix}_Programs, sizeof({prefix}_Programs) / sizeof({prefix}_Programs[0]), "
            f"{prefix}_UniqueSamples, sizeof({prefix}_UniqueSamples) / sizeof({prefix}_UniqueSamples[0]), "
            f"{fixture.aligned_pcm_bytes}u, {fixture.all_presets_aligned_pcm_bytes}u"
            "},"
        )
    lines.extend(
        [
            "};",
            "",
            "inline constexpr std::size_t kRealBankFixtureCount =",
            "    sizeof(kRealBankFixtures) / sizeof(kRealBankFixtures[0]);",
            "",
            "}  // namespace gm_synth::test_fixture",
            "",
        ]
    )
    return "\n".join(lines)


def pressure_summary(fixtures: list[BankFixture], budgets_mib: Iterable[int]) -> dict:
    budgets = [int(value) * 1024 * 1024 for value in budgets_mib]
    banks = []
    for fixture in fixtures:
        banks.append(
            {
                "name": fixture.name,
                "source_manifest": fixture.source_path,
                "program_count": len(fixture.programs),
                "unique_sample_count": len(fixture.unique_sample_ids),
                "greedy_16_aligned_pcm_bytes": fixture.aligned_pcm_bytes,
                "all_presets_aligned_pcm_bytes": fixture.all_presets_aligned_pcm_bytes,
                "budgets": [
                    {
                        "budget_bytes": budget,
                        "fits_greedy_16": fixture.aligned_pcm_bytes <= budget,
                        "greedy_headroom_bytes": budget - fixture.aligned_pcm_bytes,
                        "fits_all_presets": fixture.all_presets_aligned_pcm_bytes <= budget,
                        "all_presets_headroom_bytes": budget - fixture.all_presets_aligned_pcm_bytes,
                    }
                    for budget in budgets
                ],
            }
        )
    return {"schema_version": 1, "banks": banks}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("manifests", nargs="+", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--report", type=Path)
    parser.add_argument("--budgets-mib", nargs="*", type=int, default=[2, 4, 5, 6])
    args = parser.parse_args()

    fixtures = [load_bank_fixture(path) for path in args.manifests]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(render_cpp_header(fixtures), encoding="utf-8")

    summary = pressure_summary(fixtures, args.budgets_mib)
    print(json.dumps(summary, indent=2, ensure_ascii=False))
    if args.report is not None:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
