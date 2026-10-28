#!/usr/bin/env python3
"""Full-key synthetic V2 replay input; old analysis/startup fixtures stay frozen.

The PCM, source-loop geometry, modes and modulators are copied exactly from the
existing analysis fixture. Only the two velocity-separated continuous-loop
zones of Program 0 get a full MIDI key range. Its key-61 UntilRelease layer and
the already full-key Program 1 are retained, so this is still a layered SF2.
There is no new scorer decision, requested fade, or production-bank change.
"""
import argparse
import hashlib
from pathlib import Path
import struct
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tests.app.make_loop_analysis_fixture import children, generate as original, chunk


def generate(directory):
    with tempfile.TemporaryDirectory() as temporary:
        source_dir = Path(temporary)
        original(source_dir)
        source = (source_dir / 'fixture.sf2').read_bytes()
    lists = []
    updated = 0
    for kind, payload in children(source[12:]):
        if kind == 'LIST' and payload[:4] == b'pdta':
            parts = []
            for tag, data in children(payload[4:]):
                if tag == 'igen':
                    data = bytearray(data)
                    # The original fixture's key ranges are [60], [61], [60].
                    # Expand only [60]; preserve the distinct UntilRelease case.
                    for offset in range(0, len(data), 4):
                        op, value = struct.unpack_from('<HH', data, offset)
                        if op == 43 and value == (60 | (60 << 8)):
                            struct.pack_into('<HH', data, offset, op, 127 << 8)
                            updated += 1
                    data = bytes(data)
                parts.append(chunk(tag, data))
            payload = payload[:4] + b''.join(parts)
        lists.append(chunk(kind, payload))
    if updated != 2:
        raise ValueError('the source analysis fixture key-range contract changed')
    data = chunk('RIFF', b'sfbk' + b''.join(lists))
    if len(data) != len(source):
        raise ValueError('replay input must only replace generator values')
    digest = hashlib.sha256(data).digest()
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    lines = ['#pragma once', '#include <array>', '#include <cstdint>',
             'namespace loop_replay_fixture {',
             'alignas(4) inline constexpr std::uint8_t bytes[] = {']
    lines += [','.join(map(str, data[i:i+24])) + ',' for i in range(0, len(data), 24)]
    lines += ['};', 'inline constexpr std::array<std::uint8_t,32> digest = {' +
              ','.join(map(str, digest)) + '};',
              'inline constexpr char digest_hex[] = "' + digest.hex() + '";', '}']
    (directory / 'loop_replay_fixture.h').write_text('\n'.join(lines) + '\n')
    (directory / 'fixture.sf2').write_bytes(data)
    (directory / 'fixture.sha256').write_text(digest.hex() + '\n')
    print(f'REPLAY_FIXTURE bytes={len(data)} sha256={digest.hex()} changed_key_ranges={updated}')
    return data


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    generate(parser.parse_args().output)
