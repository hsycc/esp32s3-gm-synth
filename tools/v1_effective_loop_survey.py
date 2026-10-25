#!/usr/bin/env python3
"""Host review using production-prepared Zone geometry, not raw shdr guesses."""
import argparse
import array
from collections import Counter
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile

if __package__:
    from .v1_loop_policy import POLICY_VERSION, analyze, context_rejections
else:
    from v1_loop_policy import POLICY_VERSION, analyze, context_rejections

MAX_RECORDS = 200000
MAX_MANIFEST_BYTES = 128 * 1024 * 1024
IDENTITY_FIELDS = ("sample_id", "rate", "sample_type", "sample_link", "source_start",
                   "source_end", "start", "end", "loop_start", "loop_end", "loop_mode",
                   "noteon_address_modulated")


def digest(path):
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def read_manifest(stream):
    meta, summary, zones, rejected = None, None, [], []
    for line_number, line in enumerate(stream):
        if line_number > MAX_RECORDS or len(line) > 65536:
            raise ValueError("export manifest exceeds review bounds")
        record = json.loads(line)
        if not isinstance(record, dict):
            raise ValueError("expected JSON object")
        kind = record.get("kind")
        if line_number == 0:
            if (kind != "meta" or record.get("schema") != 1 or
                    record.get("producer") != "Sf2PlaybackProgramPreparer"):
                raise ValueError("wrong manifest producer/schema")
            meta = record
        elif summary is not None:
            raise ValueError("data after exporter summary")
        elif kind == "zone":
            zones.append(record)
        elif kind == "program_rejected":
            rejected.append(record)
        elif kind == "summary":
            summary = record
        else:
            raise ValueError("unexpected exporter record")
    if (meta is None or summary is None or summary.get("zones") != len(zones) or
            summary.get("rejected_programs") != len(rejected)):
        raise ValueError("incomplete exporter manifest")
    return meta, zones, rejected, summary


def identity(zone, bank_sha):
    payload = {key: zone[key] for key in IDENTITY_FIELDS}
    payload.update(bank_sha256=bank_sha, policy_version=POLICY_VERSION)
    return hashlib.sha256(json.dumps(payload, sort_keys=True,
                                    separators=(",", ":")).encode()).hexdigest()


def validate_zone(z, meta):
    integer_fields = ("sample_id", "rate", "sample_type", "sample_link", "source_start",
                      "source_end", "start", "end", "loop_start", "loop_end",
                      "bank", "program", "zone_index", "preset_index")
    if any(type(z.get(k)) is not int or z[k] < 0 for k in integer_fields):
        raise ValueError("invalid exported zone integer field")
    if z.get("scope") != "prepared_zone" or type(z.get("noteon_address_modulated")) is not bool:
        raise ValueError("unverified exported zone context")
    if not 0 <= z["source_start"] <= z["start"] < z["end"] <= z["source_end"] <= meta["smpl_points"]:
        raise ValueError("exported sample region outside source PCM")
    if z.get("loop_mode") != "none" and not z["start"] <= z["loop_start"] < z["loop_end"] <= z["end"]:
        raise ValueError("exported loop outside effective region")


def stratified(keys, groups, limit):
    ordered = sorted(keys, key=lambda k: (groups[k][0]["loop_end"] - groups[k][0]["loop_start"], k))
    if len(ordered) <= limit:
        return set(ordered)
    if limit == 1:
        return {ordered[len(ordered)//2]}
    return {ordered[round(i*(len(ordered)-1)/(limit-1))] for i in range(limit)}


def survey(sf2, exporter, limit=128, max_frames=200000):
    sf2, exporter = Path(sf2).resolve(), Path(exporter).resolve()
    if type(limit) is not int or not 1 <= limit <= 4096 or not 4 <= max_frames <= 1000000:
        raise ValueError("invalid survey resource bounds")
    bank_sha = digest(sf2)
    with tempfile.TemporaryFile(mode="w+", encoding="utf-8") as output:
        subprocess.run([str(exporter), str(sf2)], stdout=output, check=True, timeout=180)
        if output.tell() > MAX_MANIFEST_BYTES:
            raise ValueError("manifest too large")
        output.seek(0)
        meta, zones, rejected, summary = read_manifest(output)
    if meta.get("file_bytes") != sf2.stat().st_size:
        raise ValueError("source size changed during export")
    if any(type(meta.get(k)) is not int or meta[k] < 0 for k in ("smpl_offset", "smpl_points")):
        raise ValueError("invalid exported PCM location")
    if meta["smpl_offset"] + 2 * meta["smpl_points"] > sf2.stat().st_size:
        raise ValueError("exported smpl outside source")
    groups, plans = {}, {}
    for z in zones:
        validate_zone(z, meta)
        groups.setdefault(identity(z, bank_sha), []).append(z)
    eligible = []
    for key, references in groups.items():
        z = references[0]
        reasons = context_rejections(z)
        frames = z["loop_end"] - z["loop_start"]
        if not reasons and not 4 <= frames <= max_frames:
            reasons = ["LOOP_LENGTH_OUTSIDE_ANALYSIS_BUDGET"]
        if reasons:
            plans[key] = dict(decision="DIRECT_CONTEXT_BLOCKED", selected_fade_frames=0,
                              runtime_fade_frames=0, runtime_approved=False,
                              context_rejections=reasons, candidates=[])
        else:
            eligible.append(key)
    selected = stratified(eligible, groups, limit)
    with sf2.open("rb") as f:
        for key in sorted(selected):
            z = groups[key][0]
            count = z["loop_end"] - z["loop_start"]
            f.seek(meta["smpl_offset"] + 2*z["loop_start"])
            raw = f.read(2*count)
            if len(raw) != 2*count:
                raise ValueError("truncated effective loop PCM")
            pcm = array.array("h")
            pcm.frombytes(raw)
            if sys.byteorder != "little":
                pcm.byteswap()
            plans[key] = analyze(list(pcm), z)
    if digest(sf2) != bank_sha:
        raise ValueError("source changed during analysis; discard result")
    rows = []
    for key in sorted(groups):
        plan = plans.get(key, dict(decision="NOT_SAMPLED", selected_fade_frames=None,
                                  runtime_fade_frames=0, runtime_approved=False,
                                  candidates=[], context_rejections=["SURVEY_LIMIT"]))
        references = groups[key]
        rows.append(dict(plan_key=key, geometry={k: references[0][k] for k in IDENTITY_FIELDS},
                         references=references, **plan))
    counts = dict(sorted(Counter(r["decision"] for r in rows).items()))
    return dict(schema=2, scope="production prepared Zone loops; Host review only",
                policy_version=POLICY_VERSION, asset_sha256=bank_sha,
                exporter_sha256=digest(exporter), runtime_approved=False,
                export_summary=summary, rejected_programs=rejected,
                unique_geometries=len(groups), analyzed_geometries=len(selected),
                decision_counts=counts, rows=rows)


def report(result):
    lines = ["# Effective Zone Loop Review", "", result["scope"],
             "**No runtime plan or listening PASS is issued.**", "",
             f'Asset SHA-256: `{result["asset_sha256"]}`',
             f'Policy: `{result["policy_version"]}`',
             f'Analyzed geometries: {result["analyzed_geometries"]}/{result["unique_geometries"]}',
             f'Decisions: {result["decision_counts"]}', "",
             "| Bank/Program/Zone | Sample | Mode | Seam-best | Risk-filtered | Decision |",
             "|---|---:|---|---:|---:|---|"]
    for r in result["rows"]:
        z = r["references"][0]
        lines.append(f'| {z["bank"]}/{z["program"]}/{z["zone_index"]} | {z["sample_id"]} | '
                     f'{z["loop_mode"]} | {r.get("seam_best_fade_frames", "-")} | '
                     f'{r["selected_fade_frames"]} | {r["decision"]} |')
    lines.extend(["", "All Zone references, every analyzed candidate and its rejection reasons are in JSON.",
                  "Direct may mean no benefit, unknown geometry, or risk rejection; it is not an audio-quality PASS.",
                  "Lag/DFT gates are provisional. Preserve intentional waveform edges. No sample-ID whitelist.",
                  "Non-looping, dynamic-address and non-mono contexts are not automatically crossfaded.", ""])
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("sf2", type=Path)
    parser.add_argument("--exporter", type=Path, required=True)
    parser.add_argument("--limit", type=int, default=128)
    parser.add_argument("--max-frames", type=int, default=200000)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.output.suffix.lower() != ".json":
        parser.error("--output must end in .json (Markdown uses the same stem)")
    try:
        result = survey(args.sf2, args.exporter, args.limit, args.max_frames)
    except (OSError, ValueError, subprocess.SubprocessError) as exc:
        parser.exit(2, f"effective loop analysis failed: {exc}\n")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2, allow_nan=False)+"\n", encoding="utf-8")
    args.output.with_suffix(".md").write_text(report(result), encoding="utf-8")
    print(json.dumps({k: v for k, v in result.items() if k not in ("rows", "rejected_programs")}, indent=2))


if __name__ == "__main__":
    main()
