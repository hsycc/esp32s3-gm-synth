#!/usr/bin/env python3
"""Pinned native-SF2 inputs for the opt-in concurrent application.

The physical FM profile retains the original M7 bank and 4.5 MiB arena. Program
16 is a workload choice with a naturally selected loop; it is not a scorer
exception or a Sample-ID enable map. Admission still happens on the device.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

SYNTHETIC_SHA = '9b8bfcf0226da056839739d65b12cc145aaa2488c26aa753a2ef8a5dea8987fd'
FM_SHA = '9650eb1c2202e2d92aa3be155e4e895cf6531a241c8ea413fc09e7f9159d4a62'
PROFILES = {
    'qemu': dict(physical=False, synthetic=True, bytes=25866,
                 sha256=SYNTHETIC_SHA, arena_bytes=20480, initial_program=0,
                 replacement_program=1, blocks=2048, bank='synthetic-analysis'),
    'physical': dict(physical=True, synthetic=False, bytes=5898214,
                     sha256=FM_SHA, arena_bytes=4718592, initial_program=16,
                     replacement_program=1, blocks=32768, bank='FM-GM-M7-subset'),
}


def generate(source, output, profile, analysis=False):
    if profile not in PROFILES:
        raise ValueError('unknown realtime profile')
    spec = dict(PROFILES[profile])
    data = Path(source).read_bytes()
    if (len(data) < 12 or data[:4] != b'RIFF' or data[8:12] != b'sfbk' or
            struct.unpack_from('<I', data, 4)[0] + 8 != len(data)):
        raise ValueError('invalid SF2 container')
    digest = hashlib.sha256(data).digest()
    if len(data) != spec['bytes'] or digest.hex() != spec['sha256']:
        raise ValueError('realtime profile requires its exact pinned native SF2')
    if len(data) > 0x600000:
        raise ValueError('bank exceeds dedicated partition')
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    (output / 'bank.sf2').write_bytes(data)
    lines = ['#pragma once', '#include <array>', '#include <cstddef>',
             '#include <cstdint>', 'namespace loop_realtime_manifest {',
             f'inline constexpr char profile[]="{profile}";',
             f'inline constexpr char bank[]="{spec["bank"]}";',
             f'inline constexpr bool physical={str(spec["physical"]).lower()};',
             f'inline constexpr bool synthetic={str(spec["synthetic"]).lower()};',
             f'inline constexpr bool analysis={str(bool(analysis)).lower()};',
             f'inline constexpr std::size_t bytes={len(data)}, arena_bytes={spec["arena_bytes"]};',
             f'inline constexpr std::uint8_t initial_program={spec["initial_program"]},replacement_program={spec["replacement_program"]};',
             f'inline constexpr unsigned blocks={spec["blocks"]};',
             'inline constexpr std::array<std::uint8_t,32> digest={' + ','.join(map(str, digest)) + '};',
             f'inline constexpr char digest_hex[]="{digest.hex()}";', '}', '']
    (output / 'loop_realtime_manifest.h').write_text('\n'.join(lines))
    (output / 'bank.sha256').write_text(digest.hex() + '\n')
    manifest = dict(schema=1, profile=profile, analysis=bool(analysis), **spec,
                    runtime_approved=False, physical_timing_approved=False)
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    return digest.hex()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--profile', choices=PROFILES, required=True)
    parser.add_argument('--analysis', action='store_true')
    args = parser.parse_args()
    print(generate(args.source, args.output, args.profile, args.analysis))
