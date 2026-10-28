#!/usr/bin/env python3
"""Build-bound raw SF2 manifest. Does not authorize audible/runtime use."""
import argparse
import hashlib
from pathlib import Path
import struct

PINNED_SUBSET='9650eb1c2202e2d92aa3be155e4e895cf6531a241c8ea413fc09e7f9159d4a62'

def generate(source, output, profile, analysis=False):
    if profile not in ('qemu','physical'):
        raise ValueError('unknown profile')
    data=Path(source).read_bytes()
    if len(data)<12 or data[:4]!=b'RIFF' or data[8:12]!=b'sfbk' or struct.unpack_from('<I',data,4)[0]+8!=len(data):
        raise ValueError('invalid SF2 container')
    if len(data)>0x600000:
        raise ValueError('bank exceeds dedicated partition')
    digest=hashlib.sha256(data).digest()
    if profile=='physical' and (digest.hex()!=PINNED_SUBSET or len(data)!=5898214):
        raise ValueError('physical profile requires the pinned M7 subset, not an arbitrary bank')
    if profile=='qemu' and digest.hex()!='9b8bfcf0226da056839739d65b12cc145aaa2488c26aa753a2ef8a5dea8987fd':
        raise ValueError('unexpected synthetic QEMU bank')
    output=Path(output);output.mkdir(parents=True,exist_ok=True)
    (output/'bank.sf2').write_bytes(data)
    header='\n'.join(['#pragma once','#include <array>','#include <cstddef>','#include <cstdint>',
        'namespace loop_startup_manifest {',f'inline constexpr char profile[]="{profile}";',
        f'inline constexpr bool physical={str(profile=="physical").lower()};',
        f'inline constexpr bool analysis={str(bool(analysis)).lower()};',
        f'inline constexpr std::size_t bytes={len(data)}, arena_bytes={4718592 if profile=="physical" else 20480};',
        'inline constexpr std::uint8_t initial_program=0,replacement_program=1;',
        'inline constexpr std::array<std::uint8_t,32> digest={'+','.join(map(str,digest))+'};',
        f'inline constexpr char digest_hex[]="{digest.hex()}";','}',''])
    (output/'loop_startup_manifest.h').write_text(header)
    (output/'bank.sha256').write_text(digest.hex()+'\n')
    return digest.hex()

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('source',type=Path);p.add_argument('output',type=Path)
    p.add_argument('--profile',choices=('qemu','physical'),required=True);p.add_argument('--analysis',action='store_true')
    a=p.parse_args();print(generate(a.source,a.output,a.profile,a.analysis))
