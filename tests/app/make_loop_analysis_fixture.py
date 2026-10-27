#!/usr/bin/env python3
"""Long-loop synthetic SF2: retain the existing parser/Zone regression structure.

Only test PCM/header lengths change. Expected fades come from the FROZEN Python
policy, not from the new C++ analyzer. No production bank or whitelist changes.
"""
import argparse
import hashlib
import math
from pathlib import Path
import struct
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tests.app.make_loop_app_fixture import build as original, chunk
from tools.v1_loop_policy import analyze


def children(data):
    offset = 0
    while offset < len(data):
        size = struct.unpack_from('<I', data, offset + 4)[0]
        yield data[offset:offset+4].decode(), data[offset+8:offset+8+size]
        offset += 8 + size + (size & 1)


def generate(directory):
    frames, start, end = 4159, 23, 4119
    pcm = [[round(16000*math.sin(2*math.pi*(i-start)/period))
            for i in range(frames)] for period in (137,64,97)]
    smpl = b''.join(struct.pack('<'+'h'*frames, *p)+bytes(92) for p in pcm)
    headers = b''
    for i in range(3):
        base=i*(frames+46)
        headers+=struct.pack('<20sIIIIIBbHH', f'Analysis{i}'.encode(),base,base+frames,
                             base+start,base+end,44100,60,0,0,1)
    eos=3*(frames+46)
    headers+=struct.pack('<20sIIIIIBbHH',b'EOS',eos,eos,eos,eos,0,0,0,0,1)
    lists=[]
    for kind,payload in children(original()[12:]):
        if kind == 'LIST' and payload[:4] in (b'sdta',b'pdta'):
            parts=[]
            for tag,data in children(payload[4:]):
                parts.append(chunk(tag,smpl if tag=='smpl' else headers if tag=='shdr' else data))
            payload=payload[:4]+b''.join(parts)
        lists.append(chunk(kind,payload))
    data=chunk('RIFF',b'sfbk'+b''.join(lists));digest=hashlib.sha256(data).digest()
    context=dict(scope='prepared_zone',loop_mode='continuous',sample_type=1,noteon_address_modulated=False)
    expected=[analyze(pcm[0][start:end],context)['selected_fade_frames'],
              analyze(pcm[0][start+8:end],context)['selected_fade_frames'],
              analyze(pcm[1][start:end],context)['selected_fade_frames']]
    if expected != [32,32,0]: raise ValueError(f'frozen fixture expectations changed: {expected}')
    directory.mkdir(parents=True, exist_ok=True)
    lines=['#pragma once','#include <array>','#include <cstdint>',
           'namespace loop_analysis_fixture {','alignas(4) inline constexpr std::uint8_t bytes[] = {']
    lines += [','.join(map(str,data[i:i+24]))+',' for i in range(0,len(data),24)]
    lines += ['};','inline constexpr std::array<std::uint8_t,32> digest = {'+','.join(map(str,digest))+'};',
              'inline constexpr char digest_hex[] = "'+digest.hex()+'";', '}']
    (directory/'loop_analysis_fixture.h').write_text('\n'.join(lines)+'\n')
    (directory/'fixture.sf2').write_bytes(data)
    (directory/'fixture.sha256').write_text(digest.hex()+'\n')
    print(f'ANALYSIS_FIXTURE bytes={len(data)} sha256={digest.hex()} expected={expected}')


if __name__ == '__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('output',type=Path)
    generate(p.parse_args().output)
