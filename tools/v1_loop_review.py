#!/usr/bin/env python3
"""Audit sampled/full prepared-loop reports without changing policy or audio.

Input is review evidence, not a runtime plan. All reasons and diagnostics are
reported as provisional; even a HOST_CANDIDATE is not an audible acceptance.
"""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path

ANALYZED = {"HOST_CANDIDATE", "DIRECT_REVIEW", "DIRECT_NO_BENEFIT"}
DECISIONS = ANALYZED | {"DIRECT_CONTEXT_BLOCKED", "NOT_SAMPLED"}
PERIOD = "PERIOD_CHANGE_GE_REVIEW_LIMIT"
LAG = "LOCAL_LAG_SHIFT_REQUIRES_REVIEW"
PEAK = "NEAR_SEAM_PEAK_WORSE"
RMS = "NEAR_SEAM_RMS_WORSE"


def validated_rows(report):
    """Fail closed for inconsistent counts, duplicate identities or enabled DSP."""
    if report.get("schema") != 2 or report.get("runtime_approved") is not False:
        raise ValueError("expected disabled-runtime schema-2 prepared-loop evidence")
    rows = report.get("rows")
    if not isinstance(rows, list):
        raise ValueError("missing report rows")
    indexed = {}
    for row in rows:
        key = row.get("plan_key")
        if not isinstance(key, str) or not key or key in indexed:
            raise ValueError("missing or duplicate plan key")
        if row.get("runtime_approved") is not False or row.get("runtime_fade_frames") != 0:
            raise ValueError("review report cannot enable runtime fades")
        decision = row.get("decision")
        if decision not in DECISIONS or not row.get("references"):
            raise ValueError("invalid decision or missing Zone references")
        if decision in ANALYZED:
            candidates = row.get("candidates", [])
            fades = [c.get("fade_frames") for c in candidates]
            if fades.count(0) != 1 or len(set(fades)) != len(fades):
                raise ValueError("invalid candidate identities")
            choice = [c for c in candidates if c["fade_frames"] == row.get("selected_fade_frames")]
            if len(choice) != 1:
                raise ValueError("selected candidate absent")
            if decision == "HOST_CANDIDATE":
                if not choice[0]["fade_frames"] or choice[0].get("rejection_reasons") or choice[0].get("eligible") is not True:
                    raise ValueError("selected Host candidate is not eligible")
            elif row.get("selected_fade_frames") != 0:
                raise ValueError("Direct result has nonzero selection")
        indexed[key] = row
    counts = dict(Counter(r["decision"] for r in rows))
    if (counts != report.get("decision_counts") or
            len(rows) != report.get("unique_geometries") or
            sum(counts.get(d, 0) for d in ANALYZED) != report.get("analyzed_geometries")):
        raise ValueError("report counts disagree with rows")
    return indexed


def review_category(row):
    """Mutually exclusive descriptive groups, not a new approval policy."""
    good = [c for c in row["candidates"] if c["fade_frames"] and not c["benefit_rejections"]]
    if not good:
        raise ValueError("DIRECT_REVIEW lacks benefit-qualified alternatives")
    if all(PERIOD in c["risk_rejections"] for c in good):
        return "ALL_BENEFIT_ALTERNATIVES_PERIOD_BLOCKED"
    for reason, label in ((LAG, "LAG_ONLY_ALTERNATIVE"),
                          (PEAK, "PEAK_ONLY_ALTERNATIVE"),
                          (RMS, "RMS_ONLY_ALTERNATIVE")):
        if any(set(c["risk_rejections"]) == {reason} for c in good):
            return label
    return "COMBINED_RISKS"


def detail(row):
    return {k: row[k] for k in ("plan_key", "geometry", "references", "decision",
                                "selected_fade_frames", "runtime_fade_frames",
                                "runtime_approved", "context_rejections", "candidates")} | {
        "seam_best_fade_frames": row.get("seam_best_fade_frames"),
        "excluded_candidates": row.get("excluded_candidates", []),
    }


def audit(sampled, full, reference_ids=(), require_complete=False):
    a, b = validated_rows(sampled), validated_rows(full)
    for field in ("asset_sha256", "policy_version", "exporter_sha256", "export_summary"):
        if sampled.get(field) != full.get(field):
            raise ValueError("source, policy or exporter differs: " + field)
    if set(a) != set(b):
        raise ValueError("sampled/full geometry inventories differ")
    preserved = 0
    for key, old in a.items():
        new = b[key]
        for field in ("geometry", "references"):
            if old[field] != new[field]:
                raise ValueError("geometry/reference identity changed")
        if old["decision"] != "NOT_SAMPLED":
            if old != new:
                raise ValueError("a previously evaluated/context-blocked result changed")
            preserved += old["decision"] in ANALYZED
    if require_complete and full["decision_counts"].get("NOT_SAMPLED", 0):
        raise ValueError("full report still contains NOT_SAMPLED layouts")
    cases = []
    for row in sampled["rows"]:
        if row["decision"] in ("HOST_CANDIDATE", "DIRECT_REVIEW"):
            d = detail(row)
            d["review_category"] = ("HOST_CANDIDATE" if row["decision"] == "HOST_CANDIDATE"
                                    else review_category(row))
            cases.append(d)
    refs = []
    for sid in sorted(set(reference_ids)):
        if type(sid) is not int or not 0 <= sid <= 65535:
            raise ValueError("invalid reference Sample ID")
        matching = [r for r in full["rows"] if r["geometry"]["sample_id"] == sid]
        if not matching or any(r["decision"] == "NOT_SAMPLED" for r in matching):
            raise ValueError("reference missing or not analyzed/context-classified: " + str(sid))
        refs.append(dict(sample_id=sid, geometries=[detail(r) for r in matching]))
    return dict(schema=1, scope="prepared-loop evidence audit; not listening or firmware approval",
                runtime_approved=False, asset_sha256=sampled["asset_sha256"],
                policy_version=sampled["policy_version"],
                sampled_counts=sampled["decision_counts"], full_counts=full["decision_counts"],
                preserved_analyzed_geometries=preserved,
                sampled_review_categories=dict(Counter(c["review_category"] for c in cases
                                                       if c["decision"] == "DIRECT_REVIEW")),
                sampled_focus_cases=cases, reference_coverage=refs)


def cell(text):
    return str(text).replace("|", "/").replace("\n", " ").replace("\r", " ").replace("\x00", "")


def describe(row):
    z = row["references"][0]
    g = row["geometry"]
    return f'{g["sample_id"]} {cell(z["name"])} ({z["bank"]}/{z["program"]}/{z["zone_index"]})'


def candidate_table(row):
    lines = ["| Fade | Seam | Period fraction | Spectral | Lag | Near peak | Near RMS | Reasons |",
             "|---:|---:|---:|---:|---:|---:|---:|---|"]
    for c in row["candidates"]:
        reasons = ", ".join(c["rejection_reasons"]) or "none (Host diagnostic only)"
        values = [c.get(k) for k in ("fade_frames", "seam_step", "period_change_fraction",
                                    "spectral_distance", "lag_shift", "near_max", "near_rms")]
        lines.append("| " + " | ".join(cell(v) for v in values) + " | " + cell(reasons) + " |")
    for c in row["excluded_candidates"]:
        lines.append(f'| {c["fade_frames"]} | - | - | - | - | - | - | {cell(", ".join(c["rejection_reasons"]))} |')
    return lines


def markdown(result):
    lines = ["# Prepared Loop Candidate Audit", "",
             "**No listening PASS, no runtime enable map, no threshold changes.**", "",
             f'Policy: `{result["policy_version"]}`; asset: `{result["asset_sha256"]}`.',
             f'Sampled counts: {result["sampled_counts"]}',
             f'Full eligible-geometry counts: {result["full_counts"]}',
             f'Previous analyzed results preserved exactly: {result["preserved_analyzed_geometries"]}.',
             f'Review-case buckets (exclusive, per geometry): {result["sampled_review_categories"]}', "",
             "Same Sample ID can have different effective ranges. Counts are geometries, not unique samples.",
             "Lag and spectral thresholds are provisional. No measured pitch or perceptual claim follows.", ""]
    for row in result["sampled_focus_cases"]:
        lines += ["## " + describe(row), "",
                  f'{row["decision"]}: seam-best={row["seam_best_fade_frames"]}; '
                  f'Host-selected={row["selected_fade_frames"]}; runtime=0.',
                  "Group: " + row["review_category"], ""] + candidate_table(row) + [""]
    lines += ["# Explicit reference coverage", "",
              "Reference IDs are comparison requests, never rules in the selection algorithm.", ""]
    for group in result["reference_coverage"]:
        for row in group["geometries"]:
            g = row["geometry"]
            lines += ["## " + describe(row), "",
                      f'Effective loop [{g["loop_start"]}, {g["loop_end"]}), '
                      f'mode={g["loop_mode"]}, frames={g["loop_end"]-g["loop_start"]}.',
                      f'{row["decision"]}; Host-selected={row["selected_fade_frames"]}; runtime=0.',
                      ""] + candidate_table(row) + [""]
    return "\n".join(lines)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--sampled", required=True, type=Path)
    p.add_argument("--full", required=True, type=Path)
    p.add_argument("--sample", action="append", type=int, default=[])
    p.add_argument("--require-complete", action="store_true")
    p.add_argument("--output", required=True, type=Path)
    args = p.parse_args()
    if args.output.suffix.lower() != ".json":
        p.error("--output must end in .json")
    inputs = (args.sampled.resolve(), args.full.resolve())
    if args.output.resolve() in inputs or args.output.with_suffix(".md").resolve() in inputs:
        p.error("output must not overwrite input evidence")
    try:
        payloads = [path.read_bytes() for path in inputs]
        result = audit(*(json.loads(raw) for raw in payloads), args.sample, args.require_complete)
    except (OSError, ValueError, KeyError, TypeError) as exc:
        p.exit(2, f"loop audit failed: {exc}\n")
    result["input_sha256"] = dict(zip(("sampled", "full"),
                                      (hashlib.sha256(raw).hexdigest() for raw in payloads)))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2, allow_nan=False)+"\n", encoding="utf-8")
    args.output.with_suffix(".md").write_text(markdown(result), encoding="utf-8")
    print(json.dumps({k: v for k, v in result.items()
                      if k not in ("sampled_focus_cases", "reference_coverage")}, indent=2))


if __name__ == "__main__":
    main()
