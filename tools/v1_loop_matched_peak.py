"""Host-only peak attribution on exactly matched source edges.

Unchanged consecutive source pairs are compared to themselves, not to whatever
happened to fit in the old output window. Every blended/nonconsecutive edge is
still tested against the original Direct window maximum. A larger unchanged
source peak never raises that limit for generated edges. No PCM is published.
"""

PEAK_SCHEMA = "source-edge-matched-v1"


def point(pcm, fade, position):
    """Reference first-pass/steady-loop indexing; no whole-loop allocation."""
    n = len(pcm)
    index = position if position < n else fade + (position - n) % (n - fade)
    if fade and index >= n - fade:
        j = index - (n - fade)
        value = round((pcm[index] * (fade - j) + pcm[j] * (j + 1)) / (fade + 1))
        return max(-32768, min(32767, value)), ("blend", index, j)
    return pcm[index], ("source", index)


def matched_peak_metrics(pcm, fade, rendered, first_wrap):
    """Check every window edge and verify analytic values against the renderer.

    A pair is exempt only when BOTH samples are exact, consecutive, unchanged
    source samples. The loop join, fade entry/exit, and all mixed samples stay
    in changed_edge_peak, even when they are not the largest observed peak.
    """
    n = len(pcm)
    if (n < 4 or type(fade) is not int or not 0 <= fade < n // 2 or
            type(first_wrap) is not int or first_wrap != n):
        raise ValueError("invalid first-wrap geometry")
    radius = min(96, (n - 1) // 2)
    if len(rendered) <= n + radius:
        raise ValueError("incomplete rendered comparison window")
    positions = range(n - radius, n + radius + 1)
    direct_peak = max(abs(pcm[i % n] - pcm[(i - 1) % n]) for i in positions)
    changed_peak = unchanged_peak = shifted_peak = 0
    changed_count = unchanged_count = shifted_count = 0
    for i in positions:
        a, oa = point(pcm, fade, i - 1)
        b, ob = point(pcm, fade, i)
        if (a, b) != (rendered[i - 1], rendered[i]):
            raise ValueError("source mapping differs from reference renderer")
        magnitude = abs(b - a)
        consecutive = oa[0] == ob[0] == "source" and ob[1] == oa[1] + 1
        if consecutive:
            if (a, b) != (pcm[oa[1]], pcm[ob[1]]):
                raise ValueError("source edge is not unchanged")
            unchanged_count += 1
            unchanged_peak = max(unchanged_peak, magnitude)
            # In Direct, this same edge appears at relative offset k or k-n.
            k = ob[1]
            in_direct = -radius <= k <= radius or -radius <= k - n <= radius
            if not in_direct:
                shifted_count += 1
                shifted_peak = max(shifted_peak, magnitude)
        else:
            changed_count += 1
            changed_peak = max(changed_peak, magnitude)
    return dict(schema=PEAK_SCHEMA, source_edges_verified=True,
                reference_near_peak=direct_peak,
                candidate_near_peak=max(changed_peak, unchanged_peak),
                changed_edge_peak=changed_peak, unchanged_edge_peak=unchanged_peak,
                shifted_source_edge_peak=shifted_peak,
                changed_edges=changed_count, unchanged_edges=unchanged_count,
                shifted_source_edges=shifted_count, edge_count=2 * radius + 1)


def valid_peak_evidence(evidence, raw_peak, direct_peak, loop_frames):
    """Reject missing, nonfinite, inconsistent or partial attribution evidence."""
    if (not isinstance(evidence, dict) or evidence.get("schema") != PEAK_SCHEMA or
            evidence.get("source_edges_verified") is not True):
        return False
    magnitudes = ("reference_near_peak", "candidate_near_peak", "changed_edge_peak",
                  "unchanged_edge_peak", "shifted_source_edge_peak")
    counts = ("changed_edges", "unchanged_edges", "shifted_source_edges", "edge_count")
    if any(type(evidence.get(k)) is not int or not 0 <= evidence[k] <= 65535
           for k in magnitudes + counts):
        return False
    if type(raw_peak) is not int or type(direct_peak) is not int:
        return False
    e = evidence
    return (e["reference_near_peak"] == direct_peak and e["candidate_near_peak"] == raw_peak
            and max(e["changed_edge_peak"], e["unchanged_edge_peak"]) == raw_peak
            and e["shifted_source_edge_peak"] <= e["unchanged_edge_peak"]
            and e["shifted_source_edges"] <= e["unchanged_edges"]
            and e["changed_edges"] + e["unchanged_edges"] == e["edge_count"]
            and e["edge_count"] == 2 * min(96, (loop_frames - 1) // 2) + 1
            and (e["changed_edges"] > 0 or e["changed_edge_peak"] == 0)
            and (e["unchanged_edges"] > 0 or e["unchanged_edge_peak"] == 0)
            and (e["shifted_source_edges"] > 0 or e["shifted_source_edge_peak"] == 0))
