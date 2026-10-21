#!/usr/bin/env python3
"""Classify real-bank SF2 modulators still outside the current runtime contract.

Input is the M7_MOD_ROUTE/M7_MOD_SUMMARY stream emitted by
`test_sf2_modulator_inventory`. The current resolver's already-supported route
shapes are filtered first; only the remaining routes are classified for the
next compatibility closure batch.
"""

from __future__ import annotations

import argparse
import re
from collections import Counter
from dataclasses import dataclass
from pathlib import Path


ROUTE_RE = re.compile(
    r"^M7_MOD_ROUTE\s+asset=(?P<asset>\S+)\s+scope=(?P<scope>\S+)\s+"
    r"source=0x(?P<source>[0-9a-fA-F]+)\s+destination=(?P<destination>\d+)\s+"
    r"amount=(?P<amount>-?\d+)\s+amount_source=0x(?P<amount_source>[0-9a-fA-F]+)\s+"
    r"transform=(?P<transform>\d+)\s+count=(?P<count>\d+)\s*$"
)

PREPARED_NOTE_ON_DESTINATIONS = {7, 9, 11, 17, 28, 34, 36, 38, 48, 52}


@dataclass(frozen=True)
class Route:
    asset: str
    scope: str
    source: int
    destination: int
    amount: int
    amount_source: int
    transform: int
    count: int


def is_note_on_source(source: int) -> bool:
    midi_cc = (source & 0x0080) != 0
    index = source & 0x007F
    curve = (source >> 10) & 0x003F
    return not midi_cc and index in (2, 3) and curve <= 3


def exact(route: Route, source: int, destination: int) -> bool:
    return (
        route.source == source
        and route.destination == destination
        and route.amount_source == 0
        and route.transform == 0
    )


def is_prepared_note_on_runtime_route(route: Route) -> bool:
    if route.transform != 0:
        return False
    if not is_note_on_source(route.source):
        return False
    if route.amount_source != 0 and not is_note_on_source(route.amount_source):
        return False
    if route.destination not in PREPARED_NOTE_ON_DESTINATIONS:
        return False

    # Keep pan explicit until the runtime represents authored/stereo base pan
    # separately from the NoteOn destination delta. Otherwise the SF2 clamp
    # would be applied in the wrong order.
    return route.destination != 17


def currently_supported(route: Route) -> bool:
    if exact(route, 0x0502, 48):
        return True
    if exact(route, 0x0102, 8):
        return True
    if (
        route.source == 0x0102
        and route.destination == 8
        and route.amount_source == 0x0D02
        and route.transform == 0
        and route.amount == 0
    ):
        return True
    if exact(route, 0x0002, 11) or exact(route, 0x0102, 11):
        return True
    if exact(route, 0x0102, 9):
        return True
    if exact(route, 0x0102, 26) or exact(route, 0x0102, 34):
        return True
    if (
        route.destination == 8
        and route.amount_source == 0
        and route.transform == 0
        and is_note_on_source(route.source)
    ):
        return True
    if exact(route, 0x0587, 48):
        return True
    if exact(route, 0x028A, 17):
        return True
    if exact(route, 0x058B, 48):
        return True
    if exact(route, 0x000D, 6):
        return True
    if exact(route, 0x0081, 6):
        return True
    if exact(route, 0x00DB, 16):
        return True
    if exact(route, 0x00DD, 15):
        return True
    if is_prepared_note_on_runtime_route(route):
        return True
    return False


def classify(route: Route) -> str:
    if route.transform != 0:
        return "transform_pending"

    source_is_note_on = is_note_on_source(route.source)
    amount_source_is_note_on = route.amount_source == 0 or is_note_on_source(route.amount_source)
    if source_is_note_on and amount_source_is_note_on:
        if route.destination in (0, 4):
            return "sample_address_note_on"
        if route.destination in PREPARED_NOTE_ON_DESTINATIONS:
            return "prepared_note_on_candidate"
        return "note_on_destination_pending"

    if (route.source & 0x0080) != 0 or (
        route.amount_source != 0 and (route.amount_source & 0x0080) != 0
    ):
        return "controller_state_pending"

    return "other_source_pending"


def parse(path: Path) -> list[Route]:
    routes: list[Route] = []
    for line in path.read_text(encoding="utf-8").splitlines():
        match = ROUTE_RE.match(line)
        if not match:
            continue
        routes.append(
            Route(
                asset=match.group("asset"),
                scope=match.group("scope"),
                source=int(match.group("source"), 16),
                destination=int(match.group("destination")),
                amount=int(match.group("amount")),
                amount_source=int(match.group("amount_source"), 16),
                transform=int(match.group("transform")),
                count=int(match.group("count")),
            )
        )
    return routes


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("inventory", type=Path)
    parser.add_argument("--asset", required=True)
    args = parser.parse_args()

    routes = [route for route in parse(args.inventory) if route.asset == args.asset]
    remaining = [route for route in routes if not currently_supported(route)]
    counts: Counter[str] = Counter()
    unique: Counter[str] = Counter()
    for route in remaining:
        category = classify(route)
        counts[category] += route.count
        unique[category] += 1
        print(
            "M7_MOD_CLOSURE_ROUTE"
            f" asset={route.asset} scope={route.scope} category={category}"
            f" source=0x{route.source:04x} destination={route.destination}"
            f" amount={route.amount} amount_source=0x{route.amount_source:04x}"
            f" transform={route.transform} count={route.count}"
        )

    total_records = sum(route.count for route in remaining)
    print(
        "M7_MOD_CLOSURE_SUMMARY"
        f" asset={args.asset} remaining_records={total_records}"
        f" remaining_unique_routes={len(remaining)}"
        f" prepared_note_on_candidate={counts['prepared_note_on_candidate']}"
        f" sample_address_note_on={counts['sample_address_note_on']}"
        f" controller_state_pending={counts['controller_state_pending']}"
        f" note_on_destination_pending={counts['note_on_destination_pending']}"
        f" transform_pending={counts['transform_pending']}"
        f" other_source_pending={counts['other_source_pending']}"
    )
    for category in sorted(unique):
        print(
            "M7_MOD_CLOSURE_CATEGORY"
            f" asset={args.asset} category={category}"
            f" records={counts[category]} unique_routes={unique[category]}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
