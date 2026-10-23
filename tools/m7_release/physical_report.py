from __future__ import annotations

import argparse
import json
import pathlib
import re
from dataclasses import asdict, dataclass, field


KV_RE = re.compile(r"([A-Za-z0-9_]+)=([^\s]+)")


def _find_last_line(text: str, marker: str) -> str | None:
    found = None
    for line in text.splitlines():
        if marker in line:
            found = line
    return found


def _kv(line: str | None) -> dict[str, str]:
    if line is None:
        return {}
    return {match.group(1): match.group(2) for match in KV_RE.finditer(line)}


def _int(values: dict[str, str], key: str, default: int = -1) -> int:
    raw = values.get(key)
    if raw is None:
        return default
    try:
        return int(raw, 0)
    except ValueError:
        return default


@dataclass
class PhysicalReleaseReport:
    schema: str = "m7-physical-release-v1"
    passed: bool = False
    measured_blocks: int = 0
    render_soft_overruns: int = 0
    producer_work_soft_overruns: int = 0
    max_consecutive_work_overruns: int = 0
    max_backlog_us: int = 0
    final_backlog_us: int = 0
    min_dma_margin_us: int = 0
    hard_failure_flags: int = 0
    active_synth_queue_overflows: int = 0
    producer_stalls: int = 0
    write_timeouts: int = 0
    partial_writes: int = 0
    clipped_samples: int = 0
    stack_headroom_ok: int = 0
    normal_full_fx_max: int = 0
    pathological_heavy_max: int = 0
    music_stress_pass: bool = False
    i2s_pass: bool = False
    effects_pass: bool = False
    errors: list[str] = field(default_factory=list)


def analyze(text: str, min_blocks: int = 131072) -> PhysicalReleaseReport:
    result_line = _find_last_line(text, "i2s_qual: RESULT")
    health_line = _find_last_line(text, "i2s_qual: REALTIME_HEALTH")
    hard_line = _find_last_line(text, "i2s_qual: HARD_GATE")
    path_line = _find_last_line(text, "i2s_qual: PATH")
    envelope_line = _find_last_line(text, "physical_product_envelope: RESULT")
    music_line = _find_last_line(text, "music_stress: RESULT")

    result_kv = _kv(result_line)
    health_kv = _kv(health_line)
    hard_kv = _kv(hard_line)
    path_kv = _kv(path_line)
    envelope_kv = _kv(envelope_line)

    report = PhysicalReleaseReport()
    report.measured_blocks = _int(result_kv, "blocks", 0)
    report.render_soft_overruns = _int(health_kv, "render_soft_overruns", 0)
    report.producer_work_soft_overruns = _int(
        health_kv, "producer_work_soft_overruns", 0
    )
    report.max_consecutive_work_overruns = _int(
        health_kv, "max_consecutive_work_overruns", 0
    )
    report.max_backlog_us = _int(health_kv, "max_backlog_us", 0)
    report.final_backlog_us = _int(health_kv, "final_backlog_us", -1)
    report.min_dma_margin_us = _int(health_kv, "min_dma_margin_us", 0)
    report.hard_failure_flags = _int(hard_kv, "failure_flags", -1)
    report.active_synth_queue_overflows = _int(
        hard_kv,
        "active_synth_queue_overflows",
        _int(path_kv, "active_synth_send_queue_overflows", -1),
    )
    report.producer_stalls = _int(hard_kv, "producer_stalls", -1)
    report.write_timeouts = _int(hard_kv, "write_timeouts", -1)
    report.partial_writes = _int(hard_kv, "partial_writes", -1)
    report.clipped_samples = _int(hard_kv, "clipped_samples", -1)
    report.stack_headroom_ok = _int(hard_kv, "stack_headroom_ok", -1)
    report.normal_full_fx_max = _int(envelope_kv, "normal_full_fx_max", 0)
    report.pathological_heavy_max = _int(
        envelope_kv, "pathological_heavy_max", 0
    )
    report.music_stress_pass = bool(music_line and music_line.rstrip().endswith("PASS"))
    report.i2s_pass = _find_last_line(text, "i2s_qual: PASS") is not None
    report.effects_pass = _find_last_line(text, "effects_bench: PASS final_refs=0") is not None

    required_markers = {
        "i2s RESULT": result_line,
        "REALTIME_HEALTH": health_line,
        "HARD_GATE": hard_line,
        "PATH": path_line,
        "physical product envelope": envelope_line,
        "music stress RESULT": music_line,
    }
    for name, line in required_markers.items():
        if line is None:
            report.errors.append(f"missing {name} telemetry")

    if report.measured_blocks < min_blocks:
        report.errors.append(
            f"measured blocks {report.measured_blocks} below required {min_blocks}"
        )
    if report.hard_failure_flags != 0:
        report.errors.append(
            f"hard failure flags nonzero: 0x{report.hard_failure_flags & 0xffffffff:08x}"
        )
    if report.active_synth_queue_overflows != 0:
        report.errors.append(
            f"active synth queue overflows: {report.active_synth_queue_overflows}"
        )
    if report.producer_stalls != 0:
        report.errors.append(f"producer stalls: {report.producer_stalls}")
    if report.write_timeouts != 0:
        report.errors.append(f"write timeouts: {report.write_timeouts}")
    if report.partial_writes != 0:
        report.errors.append(f"partial writes: {report.partial_writes}")
    if report.clipped_samples != 0:
        report.errors.append(f"clipped samples: {report.clipped_samples}")
    if report.stack_headroom_ok != 1:
        report.errors.append("stack headroom gate did not pass")
    if report.final_backlog_us != 0:
        report.errors.append(f"final backlog not recovered: {report.final_backlog_us} us")
    if _int(health_kv, "backlog_exceeded_dma_window", -1) != 0:
        report.errors.append("backlog exceeded DMA window")
    if _int(health_kv, "backlog_healthy", -1) != 1:
        report.errors.append("realtime backlog health gate did not pass")
    if report.normal_full_fx_max < 24:
        report.errors.append(
            f"normal/full-FX physical polyphony regressed to {report.normal_full_fx_max}"
        )
    if report.pathological_heavy_max < 8:
        report.errors.append(
            f"heavy-DSP physical polyphony regressed to {report.pathological_heavy_max}"
        )
    if not report.music_stress_pass:
        report.errors.append("music stress did not PASS")
    if not report.i2s_pass:
        report.errors.append("I2S qualification did not PASS")
    if not report.effects_pass:
        report.errors.append("effects benchmark did not close with zero voice refs")

    fatal_markers = (
        "Guru Meditation Error",
        "panic'ed",
        "assert failed:",
        "stack smashing protect failure",
        "physical I2S/DMA qualification FAILED",
        "physical music stress qualification FAILED",
    )
    for marker in fatal_markers:
        if marker in text:
            report.errors.append(f"fatal marker present: {marker}")

    report.passed = not report.errors
    return report


def markdown(report: PhysicalReleaseReport) -> str:
    status = "PASS" if report.passed else "FAIL"
    lines = [
        "# M7 Physical Release Qualification",
        "",
        f"**Result: {status}**",
        "",
        "| Metric | Value |",
        "|---|---:|",
        f"| Measured blocks | {report.measured_blocks} |",
        f"| Render soft overruns | {report.render_soft_overruns} |",
        f"| Producer-work soft overruns | {report.producer_work_soft_overruns} |",
        f"| Max consecutive work overruns | {report.max_consecutive_work_overruns} |",
        f"| Max backlog | {report.max_backlog_us} us |",
        f"| Final backlog | {report.final_backlog_us} us |",
        f"| Minimum DMA margin | {report.min_dma_margin_us} us |",
        f"| Hard failure flags | `0x{report.hard_failure_flags & 0xffffffff:08x}` |",
        f"| Active synth queue overflows | {report.active_synth_queue_overflows} |",
        f"| Producer stalls | {report.producer_stalls} |",
        f"| Write timeouts | {report.write_timeouts} |",
        f"| Partial writes | {report.partial_writes} |",
        f"| Clipped samples | {report.clipped_samples} |",
        f"| Normal/full-FX max | {report.normal_full_fx_max} voices |",
        f"| Heavy-DSP max | {report.pathological_heavy_max} voices |",
        "",
    ]
    if report.errors:
        lines += ["## Blocking findings", ""]
        lines += [f"- {error}" for error in report.errors]
        lines.append("")
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("log", type=pathlib.Path)
    parser.add_argument("--min-blocks", type=int, default=131072)
    parser.add_argument("--json", dest="json_path", type=pathlib.Path)
    parser.add_argument("--markdown", dest="markdown_path", type=pathlib.Path)
    args = parser.parse_args()

    text = args.log.read_text(encoding="utf-8", errors="replace")
    report = analyze(text, min_blocks=args.min_blocks)

    if args.json_path:
        args.json_path.parent.mkdir(parents=True, exist_ok=True)
        args.json_path.write_text(
            json.dumps(asdict(report), indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
    if args.markdown_path:
        args.markdown_path.parent.mkdir(parents=True, exist_ok=True)
        args.markdown_path.write_text(markdown(report), encoding="utf-8")

    print(markdown(report))
    return 0 if report.passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
