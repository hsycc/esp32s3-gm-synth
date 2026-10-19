from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path
import re


_SUMMARY_RE = re.compile(
    r"i2s_worst: SUMMARY deadline_us=(?P<deadline>\d+) "
    r"deadline_candidates=(?P<candidates>\d+) retained=(?P<retained>\d+) "
    r"slowest_render_us=(?P<slowest>\d+) retained_floor_us=(?P<floor>\d+)"
)
_BLOCK_RE = re.compile(
    r"i2s_worst: BLOCK rank=(?P<rank>\d+) stream_block=(?P<stream_block>\d+) "
    r"frame=(?P<frame>\d+) render_us=(?P<render_us>\d+) events=(?P<events>\d+) "
    r"note_on=(?P<note_on>\d+) note_off=(?P<note_off>\d+) cc=(?P<cc>\d+) "
    r"program=(?P<program>\d+) pitch=(?P<pitch>\d+) pressure=(?P<pressure>\d+) "
    r"gm_reset=(?P<gm_reset>\d+) event_channels=0x(?P<event_channels>[0-9a-fA-F]{1,4}) "
    r"sustain=0x(?P<sustain>[0-9a-fA-F]{1,4}) active=(?P<active>\d+) "
    r"releasing=(?P<releasing>\d+) lowpass=(?P<lowpass>\d+) vibrato=(?P<vibrato>\d+) "
    r"mod_lfo=(?P<mod_lfo>\d+) mod_env=(?P<mod_env>\d+) fx_send=(?P<fx_send>\d+) "
    r"realtime_control=(?P<realtime_control>\d+) fast_release_tails=(?P<fast_release_tails>\d+) "
    r"steal_tails=(?P<steal_tails>\d+) exclusive_tails=(?P<exclusive_tails>\d+) "
    r"voice_channels=0x(?P<voice_channels>[0-9a-fA-F]{1,4})"
)


@dataclass(frozen=True)
class WorstBlock:
    rank: int
    stream_block: int
    frame: int
    render_us: int
    events: int
    note_on: int
    note_off: int
    control_change: int
    program_change: int
    pitch_bend: int
    channel_pressure: int
    gm_reset: int
    event_channel_mask: int
    sustain_channel_mask: int
    active_voices: int
    releasing_voices: int
    lowpass_voices: int
    vibrato_voices: int
    mod_lfo_voices: int
    mod_env_voices: int
    fx_send_voices: int
    realtime_control_voices: int
    fast_release_tails: int
    steal_tails: int
    exclusive_tails: int
    voice_channel_mask: int


@dataclass(frozen=True)
class WorstBlockReport:
    deadline_us: int | None
    deadline_candidates: int
    retained: int
    slowest_render_us: int | None
    retained_floor_us: int | None
    blocks: tuple[WorstBlock, ...]
    errors: tuple[str, ...]

    @property
    def passed(self) -> bool:
        return not self.errors


def _integer(match: re.Match[str], name: str) -> int:
    return int(match.group(name))


def _hex(match: re.Match[str], name: str) -> int:
    return int(match.group(name), 16)


def parse_log(text: str) -> WorstBlockReport:
    errors: list[str] = []
    summary = _SUMMARY_RE.search(text)
    if summary is None:
        return WorstBlockReport(
            deadline_us=None,
            deadline_candidates=0,
            retained=0,
            slowest_render_us=None,
            retained_floor_us=None,
            blocks=(),
            errors=("missing i2s_worst SUMMARY",),
        )

    deadline_us = _integer(summary, "deadline")
    candidates = _integer(summary, "candidates")
    retained = _integer(summary, "retained")
    slowest = _integer(summary, "slowest")
    floor = _integer(summary, "floor")

    blocks: list[WorstBlock] = []
    for match in _BLOCK_RE.finditer(text):
        blocks.append(
            WorstBlock(
                rank=_integer(match, "rank"),
                stream_block=_integer(match, "stream_block"),
                frame=_integer(match, "frame"),
                render_us=_integer(match, "render_us"),
                events=_integer(match, "events"),
                note_on=_integer(match, "note_on"),
                note_off=_integer(match, "note_off"),
                control_change=_integer(match, "cc"),
                program_change=_integer(match, "program"),
                pitch_bend=_integer(match, "pitch"),
                channel_pressure=_integer(match, "pressure"),
                gm_reset=_integer(match, "gm_reset"),
                event_channel_mask=_hex(match, "event_channels"),
                sustain_channel_mask=_hex(match, "sustain"),
                active_voices=_integer(match, "active"),
                releasing_voices=_integer(match, "releasing"),
                lowpass_voices=_integer(match, "lowpass"),
                vibrato_voices=_integer(match, "vibrato"),
                mod_lfo_voices=_integer(match, "mod_lfo"),
                mod_env_voices=_integer(match, "mod_env"),
                fx_send_voices=_integer(match, "fx_send"),
                realtime_control_voices=_integer(match, "realtime_control"),
                fast_release_tails=_integer(match, "fast_release_tails"),
                steal_tails=_integer(match, "steal_tails"),
                exclusive_tails=_integer(match, "exclusive_tails"),
                voice_channel_mask=_hex(match, "voice_channels"),
            )
        )

    if retained != len(blocks):
        errors.append(f"retained count mismatch: summary={retained}, blocks={len(blocks)}")
    if retained > 16:
        errors.append(f"retained count exceeds bounded Top-16 capacity: {retained}")
    if candidates < retained:
        errors.append(
            f"deadline candidates below retained blocks: candidates={candidates}, retained={retained}"
        )

    expected_ranks = list(range(1, len(blocks) + 1))
    actual_ranks = [block.rank for block in blocks]
    if actual_ranks != expected_ranks:
        errors.append(f"non-contiguous ranks: {actual_ranks}")

    render_times = [block.render_us for block in blocks]
    if render_times != sorted(render_times, reverse=True):
        errors.append("retained blocks are not sorted by descending render_us")
    if any(render_us <= deadline_us for render_us in render_times):
        errors.append("retained block does not exceed render deadline")

    if blocks:
        if blocks[0].render_us != slowest:
            errors.append(
                f"slowest summary mismatch: summary={slowest}, rank1={blocks[0].render_us}"
            )
        if blocks[-1].render_us != floor:
            errors.append(
                f"retained floor mismatch: summary={floor}, last={blocks[-1].render_us}"
            )
    elif slowest != 0 or floor != 0:
        errors.append("empty retained profile must report zero slowest/floor")

    for block in blocks:
        event_sum = (
            block.note_on
            + block.note_off
            + block.control_change
            + block.program_change
            + block.pitch_bend
            + block.channel_pressure
            + block.gm_reset
        )
        if event_sum != block.events:
            errors.append(
                f"rank {block.rank}: event subtype sum {event_sum} != total {block.events}"
            )
        for name, value in (
            ("releasing", block.releasing_voices),
            ("lowpass", block.lowpass_voices),
            ("vibrato", block.vibrato_voices),
            ("mod_lfo", block.mod_lfo_voices),
            ("mod_env", block.mod_env_voices),
            ("fx_send", block.fx_send_voices),
            ("realtime_control", block.realtime_control_voices),
        ):
            if value > block.active_voices:
                errors.append(
                    f"rank {block.rank}: {name} voices {value} exceed active {block.active_voices}"
                )

    return WorstBlockReport(
        deadline_us=deadline_us,
        deadline_candidates=candidates,
        retained=retained,
        slowest_render_us=slowest,
        retained_floor_us=floor,
        blocks=tuple(blocks),
        errors=tuple(errors),
    )


def render_markdown(report: WorstBlockReport, *, title: str = "Music stress worst blocks") -> str:
    lines = [f"## {title}"]
    if report.deadline_us is None:
        lines.append("\nProfile unavailable.")
        return "\n".join(lines)

    lines.append(
        f"\nDeadline: `{report.deadline_us} us`; candidates: `{report.deadline_candidates}`; "
        f"retained: `{report.retained}`."
    )
    if report.errors:
        lines.append("\nParser status: **INVALID**")
        for error in report.errors:
            lines.append(f"- {error}")
        return "\n".join(lines)

    lines.append("\n| Rank | Render us | Active | Release | Events | Sustain mask | Voice mask |")
    lines.append("| ---: | ---: | ---: | ---: | ---: | ---: | ---: |")
    for block in report.blocks:
        lines.append(
            f"| {block.rank} | {block.render_us} | {block.active_voices} | "
            f"{block.releasing_voices} | {block.events} | "
            f"`0x{block.sustain_channel_mask:04x}` | `0x{block.voice_channel_mask:04x}` |"
        )
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description="Parse physical music-stress worst-block logs")
    parser.add_argument("log", type=Path)
    args = parser.parse_args()
    report = parse_log(args.log.read_text(encoding="utf-8", errors="replace"))
    print(render_markdown(report))
    return 0 if report.passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
