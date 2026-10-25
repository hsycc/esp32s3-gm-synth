"""Experimental Host policy for *prepared* loop geometries, never a DSP plan.

The original planner remains available for comparison. Rejection reasons are
kept per candidate, including the seam-only winner rejected by risk checks.
Thresholds are engineering review settings, not calibrated audibility limits.
"""
from dataclasses import asdict, dataclass
import math

if __package__:
    from .v1_loop_crossfade_planner import evaluate
    from .m7_loop_seam_audio import render_loop
else:
    from v1_loop_crossfade_planner import evaluate
    from m7_loop_seam_audio import render_loop

POLICY_VERSION = "prepared-loop-review-v2"


@dataclass(frozen=True)
class Policy:
    min_seam: int = 2500
    min_absolute_improvement: int = 1800
    min_relative_improvement: float = 0.65
    max_period_fraction: float = 0.01   # >=1% requires review, not automatic use
    max_spectral_distance: float = 0.3 # provisional, NOT an audibility threshold
    max_near_peak_ratio: float = 1.0
    max_near_rms_ratio: float = 1.05


def finite_number(value):
    return (type(value) in (int, float) and math.isfinite(value))


def context_rejections(context):
    if context.get("scope") != "prepared_zone":
        return ["EFFECTIVE_GEOMETRY_UNVERIFIED"]
    if context.get("loop_mode") == "none":
        return ["NON_LOOPING_ZONE"]
    reasons = []
    if context.get("loop_mode") not in ("continuous", "until_release"):
        reasons.append("UNSUPPORTED_LOOP_MODE")
    if context.get("sample_type") != 1:
        reasons.append("NON_MONO_SAMPLE_REQUIRES_PAIR_POLICY")
    if context.get("noteon_address_modulated") is not False:
        reasons.append("NOTEON_ADDRESS_NOT_FIXED")
    return reasons


def choose(candidates, loop_frames, context, policy=Policy()):
    """Choose only among benefit-qualified candidates with no review failures.

    A nonzero result is still a Host candidate. runtime_fade_frames is always 0
    until separate rendering/audible/device qualification has been completed.
    """
    if type(loop_frames) is not int or loop_frames < 4:
        raise ValueError("invalid loop frame count")
    if not candidates or sum(c.get("fade_frames") == 0 for c in candidates) != 1:
        raise ValueError("exactly one Direct reference is required")
    fades = [c.get("fade_frames") for c in candidates]
    if any(type(f) is not int or f < 0 or f >= loop_frames // 2 for f in fades):
        raise ValueError("invalid candidate length")
    if len(set(fades)) != len(fades):
        raise ValueError("duplicate candidate lengths")
    direct = next(c for c in candidates if c["fade_frames"] == 0)
    if not finite_number(direct.get("seam_step")) or direct["seam_step"] < 0:
        raise ValueError("invalid Direct seam")
    scope_reasons = context_rejections(context)
    annotated, benefit_ok, accepted = [], [], []
    for candidate in candidates:
        c = dict(candidate)
        fade = c["fade_frames"]
        if not fade:
            c.update(benefit_rejections=[], risk_rejections=[],
                     context_rejections=[], rejection_reasons=[], eligible=True)
            annotated.append(c)
            continue
        benefit, risk = [], []
        if direct["seam_step"] < policy.min_seam:
            benefit.append("DIRECT_SEAM_BELOW_REVIEW_FLOOR")
        seam = c.get("seam_step")
        if not finite_number(seam) or seam < 0:
            benefit.append("INVALID_CANDIDATE_SEAM")
        else:
            delta = direct["seam_step"] - seam
            if delta < policy.min_absolute_improvement:
                benefit.append("ABSOLUTE_IMPROVEMENT_TOO_SMALL")
            if delta / max(1, direct["seam_step"]) < policy.min_relative_improvement:
                benefit.append("RELATIVE_IMPROVEMENT_TOO_SMALL")
        # Compare unrounded geometry; report rounding must not change a gate.
        if fade / loop_frames >= policy.max_period_fraction:
            risk.append("PERIOD_CHANGE_GE_REVIEW_LIMIT")
        distance = c.get("spectral_distance")
        if not finite_number(distance) or distance < 0:
            risk.append("SPECTRAL_UNDEFINED")
        elif distance >= policy.max_spectral_distance:
            risk.append("HIGH_LOCAL_SPECTRAL_DISTANCE")
        lag = c.get("lag_shift")
        if type(lag) is not int:
            risk.append("LAG_UNDEFINED")
        elif lag:
            risk.append("LOCAL_LAG_SHIFT_REQUIRES_REVIEW")
        for field, ratio, reason in (
            ("near_max", policy.max_near_peak_ratio, "NEAR_SEAM_PEAK_WORSE"),
            ("near_rms", policy.max_near_rms_ratio, "NEAR_SEAM_RMS_WORSE"),
        ):
            a, b = direct.get(field), c.get(field)
            if not finite_number(a) or not finite_number(b) or min(a, b) < 0:
                risk.append("NEIGHBORHOOD_METRIC_UNDEFINED")
            elif b > a * ratio:
                risk.append(reason)
        reasons = list(dict.fromkeys(scope_reasons + benefit + risk))
        c.update(benefit_rejections=benefit, risk_rejections=risk,
                 context_rejections=scope_reasons[:], rejection_reasons=reasons,
                 eligible=not reasons)
        annotated.append(c)
        if not benefit:
            benefit_ok.append(c)
        if not reasons:
            accepted.append(c)
    key = lambda c: (c["seam_step"], c["fade_frames"])
    best_seam = min(benefit_ok, key=key) if benefit_ok else direct
    selected = min(accepted, key=key) if accepted else direct
    decision = ("HOST_CANDIDATE" if selected["fade_frames"] else
                "DIRECT_CONTEXT_BLOCKED" if scope_reasons else
                "DIRECT_REVIEW" if benefit_ok else "DIRECT_NO_BENEFIT")
    return dict(policy_version=POLICY_VERSION, policy=asdict(policy),
                seam_best_fade_frames=best_seam["fade_frames"],
                selected_fade_frames=selected["fade_frames"],
                runtime_fade_frames=0, runtime_approved=False,
                decision=decision, context_rejections=scope_reasons,
                candidates=annotated)


def analyze(pcm, context, policy=Policy()):
    """Retain old metrics and add a symmetric, complete wrap neighborhood."""
    if len(pcm) < 4:
        raise ValueError("loop too short")
    if any(type(x) is not int or not -32768 <= x <= 32767 for x in pcm):
        raise ValueError("expected signed PCM16 samples")
    fades = tuple(f for f in (0, 16, 32, 64) if f < len(pcm) // 2)
    metrics = evaluate(pcm, fades)["candidates"]
    radius = min(96, (len(pcm) - 1) // 2)
    for c in metrics:
        c["legacy_one_sided_near_max"] = c["near_max"]
        c["legacy_one_sided_near_rms"] = c["near_rms"]
        out, wraps = render_loop(pcm, c["fade_frames"], len(pcm) + radius + 1)
        center = wraps[0]
        steps = [abs(out[i] - out[i - 1])
                 for i in range(center - radius, center + radius + 1)]
        c["near_max"] = max(steps)
        c["near_rms"] = math.sqrt(sum(x*x for x in steps) / len(steps))
        c["neighborhood_radius_frames"] = radius
    result = choose(metrics, len(pcm), context, policy)
    result["excluded_candidates"] = [dict(fade_frames=f, rejection_reasons=["FADE_TOO_LONG_FOR_LOOP"])
                                     for f in (16, 32, 64) if f not in fades]
    result["metric_scope"] = "complete_symmetric_wrap_neighborhood_v2"
    result["limitations"] = [
        "Thresholds are provisional review settings, not audible PASS criteria.",
        "Local DFT/lag are not a verified pitch estimate; lag=0 proves no pitch guarantee.",
        "Overlap still changes the steady loop period by fade_frames.",
        "Analysis-only; no firmware/DSP plan is approved by this report."]
    return result
