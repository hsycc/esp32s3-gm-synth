#!/usr/bin/env python3
"""Host-only bounded crossfade planner for normalized SF2 loop PCM.

No realtime DSP change. Decisions are advisory, not firmware configuration.
The seam threshold is intentionally conservative: subjective audition still
overrides automatic suggestions. The renderer's wrap/overlap semantics match
tools.m7_loop_seam_audio.render_loop.
"""
import argparse
import json
import math
from pathlib import Path

if __package__:
    from .m7_loop_seam_audio import TARGETS, FADES, render_loop, measures, sample
else:
    # Support direct CLI invocation: python3 tools/v1_loop_crossfade_planner.py
    from m7_loop_seam_audio import TARGETS, FADES, render_loop, measures, sample

# A small seam can be left untouched even if a longer fade wins numerically.
MIN_AUDIBLE_SIZED_JUMP = 2500
MIN_ABSOLUTE_IMPROVEMENT = 1800
MIN_RELATIVE_IMPROVEMENT = 0.65
MAX_PERIOD_CHANGE = 0.02


def distortion_metrics(pcm, fade):
    """Local waveform and high-frequency proxies, not a perceptual FFT score."""
    if not fade:
        return dict(overlap_change_rms=0.0, overlap_derivative_rms=0.0,
                    derivative_ratio=1.0)
    tail = pcm[-fade:]
    head = pcm[:fade]
    mixed = [
        round((tail[i] * (fade - i) + head[i] * (i + 1)) / (fade + 1))
        for i in range(fade)
    ]
    difference = [mixed[i] - tail[i] for i in range(fade)]
    delta = [mixed[i] - mixed[i-1] for i in range(1, fade)]
    original_delta = [tail[i] - tail[i-1] for i in range(1, fade)]
    rms = lambda xs: math.sqrt(sum(x*x for x in xs) / len(xs)) if xs else 0.0
    base = rms(original_delta)
    return dict(overlap_change_rms=round(rms(difference), 2),
                overlap_derivative_rms=round(rms(delta), 2),
                derivative_ratio=round(rms(delta) / base, 4) if base else None)


def evaluate(pcm, fades=FADES):
    """Return decision and per-candidate objective diagnostics.

    This preliminary score considers the exact seam step and period change,
    not perceptual spectral similarity. It never claims a listening PASS.
    """
    if len(pcm) < 4:
        raise ValueError("loop too short")
    if 0 not in fades:
        raise ValueError("direct reference required")
    if len(set(fades)) != len(fades):
        raise ValueError("duplicate candidates")
    if any(not isinstance(x, int) or x < 0 or x >= len(pcm)//2 for x in fades):
        raise ValueError("fade outside bounded loop")
    # Enough output for at least one wrap and a post-wrap sample.
    length = len(pcm) + 1
    candidates = []
    for fade in fades:
        out, wraps = render_loop(pcm, fade, length)
        metrics = measures(out, wraps, fade)
        period_change = fade / len(pcm)
        # The near-wrap RMS and max are preserved for later analysis,
        # but the seam step is intentionally the primary decision metric.
        candidates.append(dict(fade_frames=fade,
                               seam_step=metrics["wrap_step_pcm16"],
                               near_max=metrics["max_near_wrap_step_pcm16"],
                               near_rms=metrics["rms_near_wrap_step_pcm16"],
                               period_change_fraction=round(period_change, 8),
                               **distortion_metrics(pcm, fade)))
    direct = next(c for c in candidates if c["fade_frames"] == 0)
    eligible = []
    for c in candidates:
        if c["fade_frames"] == 0 or c["period_change_fraction"] > MAX_PERIOD_CHANGE:
            continue
        improvement = direct["seam_step"] - c["seam_step"]
        if (direct["seam_step"] >= MIN_AUDIBLE_SIZED_JUMP and
                improvement >= MIN_ABSOLUTE_IMPROVEMENT and
                improvement / max(1, direct["seam_step"]) >= MIN_RELATIVE_IMPROVEMENT):
            eligible.append(c)
    # Choose the smallest residual seam; use shorter fade for ties.
    best = min(eligible, key=lambda c: (c["seam_step"], c["fade_frames"])) if eligible else direct
    return {
        "selected_fade_frames": best["fade_frames"],
        "decision": "CANDIDATE_ONLY" if best["fade_frames"] else "DIRECT",
        "reason": "eligible seam reduction; audition required" if best["fade_frames"] else
                  "no candidate meets conservative seam and period gates",
        "candidates": candidates,
        "limitations": [
            "waveform and first-difference proxies are not pitch, FFT spectra, or perceptual quality",
            "overlap shortens loop period by fade_frames",
            "no firmware/VoiceRenderer change and no audible PASS implied",
        ],
    }


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("sf2", type=Path)
    p.add_argument("--output", type=Path, required=True)
    args = p.parse_args()
    results = []
    for target in TARGETS:
        rate, pcm = sample(args.sf2, target)
        result = evaluate(pcm)
        results.append({"sample_id": target[0], "rate": rate, "loop_frames": len(pcm), **result})
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(results, indent=2) + "\n", encoding="utf-8")
    report = ["# Dynamic Crossfade Host Analysis", "",
              "Advisory only; not a listening PASS, not embedded DSP.", "",
              "| Sample | Selected | Direct seam | Selected seam | Period change | Overlap change RMS | Derivative ratio |",
              "|---:|---:|---:|---:|---:|---:|---:|"]
    for row in results:
        direct = next(c for c in row["candidates"] if c["fade_frames"] == 0)
        chosen = next(c for c in row["candidates"] if c["fade_frames"] == row["selected_fade_frames"])
        report.append(
            f'| {row["sample_id"]} | {chosen["fade_frames"]}f | '
            f'{direct["seam_step"]} | {chosen["seam_step"]} | '
            f'{chosen["period_change_fraction"]:.4%} | '
            f'{chosen["overlap_change_rms"]:.2f} | {chosen["derivative_ratio"]} |')
    report.extend(["", "First-difference ratio is a local high-frequency proxy, not FFT, pitch or a listening score.",
                   "Selected candidates require separate audible validation."])
    args.output.with_suffix(".md").write_text("\n".join(report) + "\n", encoding="utf-8")
    print(json.dumps([{"sample_id": r["sample_id"],
                       "selected_fade_frames": r["selected_fade_frames"],
                       "decision": r["decision"]} for r in results], indent=2))


if __name__ == "__main__":
    main()
