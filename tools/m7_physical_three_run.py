#!/usr/bin/env python3
"""Compare exactly three M7 N16R8 physical logs from one instrumented firmware SHA.

Does not infer performance from CI or turn isolated soft deadline misses into
hard failures. Run after collecting three independent full boot logs.
"""
import argparse
import json
import re
from pathlib import Path

def line_with(lines, marker):
    return [x for x in lines if marker in x]

def fields(line):
    return dict(re.findall(r"([A-Za-z][A-Za-z0-9_]*)=([^\s]+)", line))

def expect_int(record, field, base=10):
    if field not in record:
        raise ValueError("missing field: " + field)
    return int(record[field], base)

def parse_log(path):
    lines = path.read_text(errors="replace").splitlines()
    versions = [m.group(1) for ln in lines
                if (m := re.search(r"App version:\s*([0-9a-fA-F]+)",ln))]
    if not versions:
        raise ValueError(f"{path}: App version missing")
    result_lines = line_with(lines,"i2s_qual: RESULT blocks=")
    health_lines = line_with(lines,"i2s_qual: REALTIME_HEALTH ")
    gate_lines = line_with(lines,"i2s_qual: HARD_GATE ")
    if len(result_lines) != 2 or len(health_lines) != 2 or len(gate_lines) != 2:
        raise ValueError(f"{path}: expected two complete I2S result/health/gate phases")
    render = fields(result_lines[-1])
    health = fields(health_lines[-1])
    gate = fields(gate_lines[-1])
    worst_lines = line_with(lines,"i2s_worst: BLOCK rank=1 ")
    stage_lines = line_with(lines,"i2s_worst: STAGES rank=1 ")
    burst_lines = line_with(lines,"music_stress: NOTE_ON_BURST ")
    filter_lines = line_with(lines,"music_stress: CC21_FILTER ")
    music_lines = line_with(lines,"music_stress: RESULT ")
    poly_lines = line_with(lines,"physical_polyphony: RESULT ")
    if not all((worst_lines,stage_lines,burst_lines,filter_lines,music_lines,poly_lines)):
        raise ValueError(f"{path}: instrumented music stress diagnostics missing")
    worst, stage, burst, cc21 = map(lambda a: fields(a[-1]), 
                                   (worst_lines,stage_lines,burst_lines,filter_lines))
    render_timings = render.get("render_us[min/avg/max]")
    if not render_timings or len(render_timings.split("/")) != 3:
        raise ValueError(f"{path}: render timings missing")
    minimum,average,maximum = map(int,render_timings.split("/"))
    if "WDT" in "\n".join(x for x in lines if "task_wdt:" in x or "Guru Meditation" in x):
        raise ValueError(f"{path}: crash/WDT detected")
    passes = {
        "hard_gate": expect_int(gate,"failure_flags",16) == 0,
        "backlog": health.get("backlog_healthy") == "1" and
                   expect_int(health,"final_backlog_us") == 0,
        "music": " PASS" in music_lines[-1],
        "polyphony": " PASS" in poly_lines[-1],
        "cc21_sweep": expect_int(cc21,"filter_changes") >= 3,
        "burst_attribution": expect_int(burst,"notes") >= 24 and
                             expect_int(stage,"stage_valid") == 1,
    }
    return {
        "file": path.name, "app_version": versions[0],
        "render_min_us": minimum, "render_avg_us": average,
        "render_max_us": maximum,
        "render_soft_overruns": expect_int(health,"render_soft_overruns"),
        "max_backlog_us": expect_int(health,"max_backlog_us"),
        "dma_margin_us": expect_int(health,"min_dma_margin_us"),
        "hard_gate_flags": gate["failure_flags"],
        "worst_block_frame": expect_int(worst,"frame"),
        "worst_block_events": expect_int(worst,"events"),
        "worst_block_note_on": expect_int(worst,"note_on"),
        "stage_setup_us": expect_int(stage,"setup_prepare_us"),
        "stage_event_apply_us": expect_int(stage,"event_apply_us"),
        "stage_voice_render_us": expect_int(stage,"voice_render_us"),
        "stage_finalize_us": expect_int(stage,"effects_finalize_us"),
        "burst_notes": expect_int(burst,"notes"),
        "burst_zone_resolve_total_us": expect_int(burst,"zone_resolve_total_us"),
        "burst_voice_start_total_us": expect_int(burst,"voice_start_total_us"),
        "filter_changes": expect_int(cc21,"filter_changes"),
        "checks": passes
    }

def report(runs):
    if len(runs) != 3:
        raise ValueError("require exactly three independent physical runs")
    versions={r["app_version"] for r in runs}
    if len(versions)!=1:
        raise ValueError("firmware App version differs across runs")
    lines=["# M7 three-run N16R8 physical comparison","",
           f"App version: \`{next(iter(versions))}\`",
           "Note: timing includes qualification instrumentation; compare against an instrumented baseline.",
           "",
           "| Run | Render avg/max (us) | Soft overruns | Max backlog (us) | DMA margin (us) | Zone resolve burst (us) | Voice start burst (us) | CC21 changes | Gate |",
           "|---|---:|---:|---:|---:|---:|---:|---:|---|"]
    all_pass=True
    for r in runs:
        passed=all(r["checks"].values())
        all_pass &= passed
        lines.append(f"| {r['file']} | {r['render_avg_us']}/{r['render_max_us']} | "
                     f"{r['render_soft_overruns']} | {r['max_backlog_us']} | "
                     f"{r['dma_margin_us']} | {r['burst_zone_resolve_total_us']} | "
                     f"{r['burst_voice_start_total_us']} | {r['filter_changes']} | "
                     f"{'PASS' if passed else 'FAIL'} |")
    lines += ["",f"**Automated structural/hard-gate checks: {'PASS' if all_pass else 'FAIL'}**",
              "A PASS does not replace listening or prove the absence of audible clicks.",
              "Soft overruns are reported, not hidden or treated as DMA underruns.",""]
    return "\n".join(lines)

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("logs", nargs=3, type=Path)
    p.add_argument("--output",type=Path)
    args=p.parse_args()
    runs=[parse_log(p) for p in args.logs]
    md=report(runs)
    if args.output:
        args.output.mkdir(parents=True,exist_ok=True)
        (args.output/"physical-three-run.md").write_text(md)
        (args.output/"physical-three-run.json").write_text(json.dumps(runs,indent=2)+"\n")
    print(md)
    if not all(all(r["checks"].values()) for r in runs):
        raise SystemExit(1)

if __name__=="__main__":
    main()
