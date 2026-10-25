#!/usr/bin/env python3
"""Host-only bounded crossfade planner for normalized SF2 loop PCM.

No realtime DSP change. Decisions are advisory, not firmware configuration.
The seam threshold is intentionally conservative: subjective audition still
overrides automatic suggestions. The renderer's wrap/overlap semantics match
tools.m7_loop_seam_audio.render_loop.
"""
import argparse
import json
from pathlib import Path

from tools.m7_loop_seam_audio import TARGETS, FADES, render_loop, measures, sample

# A small seam can be left untouched even if a longer fade wins numerically.
MIN_AUDIBLE_SIZED_JUMP = 2500
MIN_ABSOLUTE_IMPROVEMENT = 1800
MIN_RELATIVE_IMPROVEMENT = 0.65
MAX_PERIOD_CHANGE = 0.02


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
                               period_change_fraction=round(period_change, 8)))
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
            "heuristic does not measure pitch, spectrum, or perceptual quality",
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
    print(json.dumps([{"sample_id": r["sample_id"],
                       "selected_fade_frames": r["selected_fade_frames"],
                       "decision": r["decision"]} for r in results], indent=2))


if __name__ == "__main__":
    main()
