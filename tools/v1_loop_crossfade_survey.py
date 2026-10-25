#!/usr/bin/env python3
"""Bounded, deterministic survey of raw SF2 shdr loop regions (host only).

This scans SF2 sample headers, NOT preset/instrument zone effective start/end
offsets. Recommendations are diagnostics, not embedded loop plans.
"""
import argparse
import array
import collections
import json
import struct
import sys
from pathlib import Path

if __package__:
    from .m7_loop_seam_audio import chunks
    from .v1_loop_crossfade_planner import evaluate
else:
    from m7_loop_seam_audio import chunks
    from v1_loop_crossfade_planner import evaluate

HEADER = struct.Struct("<20sIIIIIBbHH")
MAX_FRAMES = 200000


def sample_headers(sf2):
    (pcm_offset, pcm_bytes), (shdr_offset, shdr_bytes) = chunks(sf2)
    if shdr_bytes % HEADER.size or shdr_bytes < HEADER.size:
        raise ValueError("invalid SF2 shdr table")
    with sf2.open("rb") as f:
        f.seek(shdr_offset)
        raw = f.read(shdr_bytes)
    if len(raw) != shdr_bytes:
        raise ValueError("truncated sample headers")
    result = []
    for sample_id in range(shdr_bytes // HEADER.size - 1):  # exclude EOS
        name, start, end, loop_start, loop_end, rate, _, _, _, _ = (
            HEADER.unpack_from(raw, sample_id*HEADER.size))
        if not (0 <= start < end and start <= loop_start < loop_end <= end and
                8000 <= rate <= 192000 and end*2 <= pcm_bytes):
            continue
        frames = loop_end-loop_start
        if frames <= 128:
            continue
        result.append(dict(sample_id=sample_id,
                           name=name.split(b"\\x00", 1)[0].decode("latin1"),
                           loop_start=loop_start, loop_end=loop_end,
                           loop_frames=frames, rate=rate))
    return pcm_offset, result


def selected_headers(headers, limit):
    """Deterministic coverage across short, medium and long loop regions."""
    if limit <= 0:
        raise ValueError("limit must be positive")
    ordered = sorted(headers, key=lambda h: (h["loop_frames"], h["sample_id"]))
    if len(ordered) <= limit:
        return sorted(ordered, key=lambda h: h["sample_id"])
    if limit == 1:
        return [ordered[len(ordered)//2]]
    indexes = sorted(set(round(i*(len(ordered)-1)/(limit-1))
                         for i in range(limit)))
    return sorted((ordered[i] for i in indexes), key=lambda h: h["sample_id"])


def classify(row):
    chosen = next(c for c in row["candidates"]
                  if c["fade_frames"] == row["selected_fade_frames"])
    reasons=[]
    # Review triggers are descriptive triage defaults; no calibrated cutoff.
    if chosen["fade_frames"] and chosen["spectral_distance"] is None:
        reasons.append("SPECTRAL_UNDEFINED")
    if chosen["fade_frames"] and chosen["spectral_distance"] is not None and chosen["spectral_distance"] >= 0.3:
        reasons.append("HIGH_LOCAL_SPECTRAL_DISTANCE")
    if chosen["fade_frames"] and chosen["lag_shift"] not in (None, 0):
        reasons.append("LOCAL_LAG_SHIFT")
    if chosen["fade_frames"] and chosen["period_change_fraction"] >= 0.01:
        reasons.append("PERIOD_CHANGE_GE_1PCT")
    return reasons


def survey(sf2, limit=128, max_frames=MAX_FRAMES):
    if max_frames <= 128:
        raise ValueError("max_frames must exceed 128")
    pcm_offset, headers = sample_headers(sf2)
    selected = selected_headers(headers, limit)
    rows = []
    skipped = collections.Counter()
    with sf2.open("rb") as f:
        for h in selected:
            if h["loop_frames"] > max_frames:
                skipped["LOOP_TOO_LONG"] += 1
                continue
            f.seek(pcm_offset + 2*h["loop_start"])
            raw = f.read(2*h["loop_frames"])
            if len(raw) != 2*h["loop_frames"]:
                skipped["TRUNCATED_PCM"] += 1
                continue
            pcm = array.array("h")
            pcm.frombytes(raw)
            if sys.byteorder != "little":
                pcm.byteswap()
            candidates = tuple(x for x in (0, 16, 32, 64)
                               if x == 0 or x < len(pcm)//2)
            result = evaluate(pcm, candidates)
            chosen = next(c for c in result["candidates"]
                          if c["fade_frames"] == result["selected_fade_frames"])
            direct = next(c for c in result["candidates"]
                          if c["fade_frames"] == 0)
            rows.append({**h, "selected_fade_frames": result["selected_fade_frames"],
                         "decision": result["decision"],
                         "direct_seam": direct["seam_step"],
                         "selected_seam": chosen["seam_step"],
                         "spectral_distance": chosen["spectral_distance"],
                         "lag_shift": chosen["lag_shift"],
                         "period_change_fraction": chosen["period_change_fraction"],
                         "review_flags": classify(result)})
    counts = dict(sorted(collections.Counter(
        str(r["selected_fade_frames"]) for r in rows).items()))
    return {"scope": "raw SF2 shdr loops only; not effective preset/instrument zone offsets",
            "evaluated": len(rows), "eligible_headers": len(headers),
            "selected_headers": len(selected), "skip_counts": dict(skipped),
            "fade_counts": counts, "rows": rows}


def report(result):
    lines = ["# SF2 Loop Crossfade Survey", "",
             "**Advisory, not audible PASS and not a firmware plan.**",
             result["scope"], "",
             f'Evaluated: {result["evaluated"]}; eligible headers: {result["eligible_headers"]}; '
             f'selected: {result["selected_headers"]}.',
             f'Fade distribution: {result["fade_counts"]}; skipped: {result["skip_counts"]}.',
             "", "| ID | Name | Loop frames | Fade | Seam before / after | Spectral | Lag | Review |",
             "|---:|---|---:|---:|---:|---:|---:|---|"]
    for r in result["rows"]:
        lines.append(
            f'| {r["sample_id"]} | {r["name"].replace("|", "/")} | '
            f'{r["loop_frames"]} | {r["selected_fade_frames"]}f | '
            f'{r["direct_seam"]} / {r["selected_seam"]} | '
            f'{r["spectral_distance"]} | {r["lag_shift"]} | '
            f'{", ".join(r["review_flags"]) or "-"} |')
    lines.extend(["", "Review thresholds are provisional flags only.",
                  "Sample 604/602/62 prior audition geometries include normalized playback starts,",
                  "whereas this survey uses raw shdr loopStart; outputs are not directly comparable.", ""])
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("sf2", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--limit", type=int, default=128)
    parser.add_argument("--max-frames", type=int, default=MAX_FRAMES)
    args = parser.parse_args()
    result=survey(args.sf2, args.limit, args.max_frames)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2)+"\n", encoding="utf-8")
    args.output.with_suffix(".md").write_text(report(result), encoding="utf-8")
    print(json.dumps({k:v for k,v in result.items() if k != "rows"}, indent=2))


if __name__ == "__main__":
    main()
