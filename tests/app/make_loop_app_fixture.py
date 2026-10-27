#!/usr/bin/env python3
"""Deterministic synthetic SF2 used by actual production parser/service tests."""
import argparse
import hashlib
from pathlib import Path
import struct


def chunk(name, data):
    return name.encode() + struct.pack('<I', len(data)) + data + (b'\0' if len(data) % 2 else b'')


def build():
    pcm = [((i * 997 + 143) % 50001) - 25000 for i in range(640)]
    data = b''.join(struct.pack('<h', x) for x in pcm) + bytes(92)
    smpl = data * 3
    phdr = b''.join(struct.pack('<20sHHHIII', name, program, 0, bag, 0, 0, 0)
                    for name, program, bag in ((b'Layered', 0, 0), (b'Replacement', 1, 1), (b'EOP', 0, 2)))
    pbag = b''.join(struct.pack('<HH', i, 0) for i in range(3))
    pgen = struct.pack('<HHHHHH', 41, 0, 41, 1, 0, 0)
    inst = b''.join(struct.pack('<20sH', name, idx) for name, idx in ((b'Layered', 0), (b'Replacement', 3), (b'EOI', 4)))
    # Two layers share sample0 AND effective geometry but differ by velocity.
    # A third layer shares sample0 with a distinct loop range, to catch any
    # sample-ID-only selection. Selection key/vel differs from fixedVelocity.
    zones = [
        [(43, 60 | (60 << 8)), (44, 80 | (127 << 8)), (47, 20), (54, 1), (53, 0)],
        [(43, 61 | (61 << 8)), (44, 1 | (127 << 8)), (54, 3), (53, 0)],
        [(43, 60 | (60 << 8)), (44, 1 | (79 << 8)), (2, 8), (54, 1), (53, 0)],
        [(54, 1), (53, 1)],
    ]
    ibag, igen = b'', b''
    count = 0
    for zone in zones:
        ibag += struct.pack('<HH', count, 0)
        for op, value in zone:
            igen += struct.pack('<HH', op, value)
            count += 1
    ibag += struct.pack('<HH', count, 0)
    igen += bytes(4)
    shdr = b''
    for i in range(3):
        start = i * 686
        shdr += struct.pack('<20sIIIIIBbHH', f'Fixture{i}'.encode(), start, start + 640,
                            start + 23, start + 595, 44100, 60, 0, 0, 1)
    shdr += struct.pack('<20sIIIIIBbHH', b'EOS', 2058, 2058, 2058, 2058, 0, 0, 0, 0, 1)
    pdta = b''.join(chunk(n, v) for n, v in [('phdr', phdr), ('pbag', pbag), ('pmod', bytes(10)),
                ('pgen', pgen), ('inst', inst), ('ibag', ibag), ('imod', bytes(10)), ('igen', igen), ('shdr', shdr)])
    return chunk('RIFF', b'sfbk' + chunk('LIST', b'INFO' + chunk('ifil', struct.pack('<HH', 2, 1))) +
                 chunk('LIST', b'sdta' + chunk('smpl', smpl)) + chunk('LIST', b'pdta' + pdta))

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    payload = build()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(payload)
    print(hashlib.sha256(payload).hexdigest())
