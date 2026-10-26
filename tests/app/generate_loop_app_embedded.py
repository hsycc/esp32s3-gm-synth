#!/usr/bin/env python3
"""Generate immutable synthetic test bytes plus an independently computed digest."""
import argparse
import hashlib
from pathlib import Path
from make_loop_app_fixture import build


def generate(directory):
    data = build()
    digest = hashlib.sha256(data).digest()
    # Freeze the earlier application fixture, not an arbitrary new test bank.
    if digest.hex() != 'e1c3af71233c2e6b6151e53dbca19ce7bd05e0f14d160cf3ed11a2da419af51f':
        raise ValueError('frozen application fixture changed')
    directory.mkdir(parents=True, exist_ok=True)
    lines = ['#pragma once', '#include <cstdint>', '#include <array>',
             'namespace loop_app_fixture {', 'alignas(4) inline constexpr std::uint8_t bytes[] = {']
    lines += [','.join(str(x) for x in data[i:i+24]) + ',' for i in range(0, len(data), 24)]
    lines += ['};', 'inline constexpr std::array<std::uint8_t,32> digest = {' + ','.join(map(str,digest)) + '};',
              'inline constexpr char digest_hex[] = "' + digest.hex() + '";', '}']
    (directory/'loop_app_fixture.h').write_text('\n'.join(lines)+'\n')
    (directory/'fixture.sha256').write_text(digest.hex()+'\n')
    print(f'APP_EMBEDDED bytes={len(data)} sha256={digest.hex()}')

if __name__ == '__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('output', type=Path)
    generate(p.parse_args().output)
