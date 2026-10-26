#!/usr/bin/env python3
"""Fail-closed compile/ELF and native-vs-Xtensa QEMU evidence checks.

No hardware timing, memory budget or audible approval follows from these gates.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import struct

MACRO = 'GM_SYNTH_EXPERIMENTAL_VOICE_LOOP'
REQUIRED = {
    'firmware/voice/VoiceRenderer.cpp',
    'firmware/voice/VoiceRuntimeControl.cpp',
    'firmware/voice/VoiceEffectsBus.cpp',
    'firmware/gm/ZoneResolver.cpp',
    'firmware/sample/PublishedSampleDirectory.cpp',
}
FATAL = re.compile(r'V1_TARGET_FAIL|Guru Meditation|assert(?:ion)? failed|Assertion .*failed|abort\(\)|panic.ed|task_wdt:')
RESULT = re.compile(r'^V1_TARGET_RESULT cases=(\d+) frames=(\d+) hash=(\d+)$', re.M)
LAYOUT = re.compile(r'^V1_TARGET_LAYOUT pointer=(\d+) voice=(\d+) params=(\d+) pool=(\d+)$', re.M)


def expand_response_files(tokens, directory, records, stack=()):
    """Expand GCC @files in compiler working-directory coordinates, fail closed.

    Only generated files below that build directory are accepted. Nested
    expansion is bounded and cycles, missing files and hidden -D/-U conflicts
    remain errors, rather than treating response files as harmless text.
    """
    directory = Path(directory).resolve()
    expanded = []
    for token in tokens:
        if not token.startswith('@'):
            expanded.append(token)
            continue
        if len(stack) >= 4:
            raise ValueError('response-file nesting exceeds bound')
        path = Path(token[1:])
        if not path.is_absolute():
            path = directory / path
        path = path.resolve()
        try:
            path.relative_to(directory)
        except ValueError as exc:
            raise ValueError('response file outside compiler build directory') from exc
        if path in stack:
            raise ValueError('cyclic response file')
        if path.stat().st_size > 1048576:
            raise ValueError('response file exceeds size bound')
        raw = path.read_bytes()
        if len(records) >= 64 and str(path) not in records:
            raise ValueError('too many response files')
        records[str(path)] = dict(bytes=len(raw), sha256=hashlib.sha256(raw).hexdigest())
        expanded.extend(expand_response_files(shlex.split(raw.decode('utf-8')),
                                               directory, records, stack + (path,)))
        if len(expanded) > 100000:
            raise ValueError('expanded compiler command exceeds bound')
    return expanded


def audit_build(root, database, elf, app_prefix):
    root = Path(root).resolve()
    entries = json.loads(Path(database).read_text(encoding='utf-8'))
    if not isinstance(entries, list) or not entries:
        raise ValueError('empty or invalid compile database')
    owned = []
    response_files = {}
    app_count = 0
    for entry in entries:
        directory = Path(entry['directory'])
        source = Path(entry['file'])
        if not source.is_absolute():
            source = directory / source
        try:
            relative = source.resolve().relative_to(root).as_posix()
        except ValueError:
            continue
        if source.suffix not in ('.cc', '.cpp', '.cxx'):
            continue
        if not (relative.startswith('firmware/') or relative.startswith(app_prefix.rstrip('/') + '/')):
            continue
        tokens = entry.get('arguments')
        if tokens is None:
            tokens = shlex.split(entry['command'])
        if not tokens:
            raise ValueError(f'missing compiler arguments: {relative}')
        tokens = expand_response_files(tokens, directory, response_files)
        if not any(Path(t).name.startswith('xtensa-') and Path(t).name.endswith('g++') for t in tokens):
            raise ValueError(f'not an Xtensa C++ compile: {relative}')
        definitions, undefines = [], []
        i = 0
        while i < len(tokens):
            token = tokens[i]
            if token in ('-D', '-U'):
                i += 1
                if i == len(tokens):
                    raise ValueError('missing preprocessor argument')
                operand = tokens[i]
                kind = token
            elif token.startswith(('-D', '-U')):
                kind, operand = token[:2], token[2:]
            else:
                i += 1
                continue
            name, separator, value = operand.partition('=')
            if name == MACRO:
                if kind == '-U':
                    undefines.append(operand)
                else:
                    definitions.append(value if separator else '1')
            i += 1
        if undefines or not definitions or any(v != '1' for v in definitions):
            raise ValueError(f'inconsistent or missing enabled macro: {relative}')
        app_count += relative.startswith(app_prefix.rstrip('/') + '/')
        owned.append(relative)
    if not REQUIRED.issubset(owned) or not app_count:
        raise ValueError(f'missing production/app compilation: {sorted(REQUIRED - set(owned))}')
    header = Path(elf).read_bytes()[:20]
    if len(header) < 20 or header[:6] != b'\x7fELF\x01\x01':
        raise ValueError('expected a 32-bit little-endian ELF')
    kind, machine = struct.unpack_from('<HH', header, 16)
    if kind != 2 or machine != 94:  # ET_EXEC / EM_XTENSA
        raise ValueError('not a linked Xtensa executable')
    return dict(schema=1, scope='enabled target compilation/link only',
                runtime_approved=False, macro=MACRO, value=1,
                owned_cpp_count=len(owned), app_cpp_count=app_count,
                translation_units=sorted(owned), response_files=response_files, elf_machine=machine)


def compare_logs(native_text, target_text):
    for text in (native_text, target_text):
        if FATAL.search(text):
            raise ValueError('fatal marker in execution log')
    native = RESULT.findall(native_text)
    target = RESULT.findall(target_text)
    sizes = LAYOUT.findall(target_text)
    if len(native) != 1 or len(target) != 1 or native != target:
        raise ValueError('missing, duplicate or mismatched PCM/state summary')
    if target_text.count('V1_TARGET_BEGIN esp32s3 macro=1 synthetic_only=1') != 1 or target_text.count('V1_TARGET_PASS') != 1:
        raise ValueError('missing/duplicate target execution markers')
    if not (target_text.index('V1_TARGET_BEGIN') < target_text.index('V1_TARGET_RESULT') < target_text.index('V1_TARGET_PASS')):
        raise ValueError('invalid target marker order')
    cases, frames, checksum = map(int, target[0])
    if cases != 72 or frames <= 0 or not 0 <= checksum < 1 << 64:
        raise ValueError('invalid execution coverage')
    if len(sizes) != 1 or int(sizes[0][0]) != 4 or any(int(v) <= 0 for v in sizes[0]):
        raise ValueError('missing or invalid Xtensa layout')
    return dict(schema=1, scope='synthetic production Voice executed in ESP32-S3 QEMU',
                runtime_approved=False, hardware_timing_approved=False,
                cases=cases, compared_voice_frames=frames, pcm_trace_hash=checksum,
                native_target_equal=True,
                target_layout=dict(zip(('pointer', 'voice', 'params', 'pool'), map(int, sizes[0]))))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='mode', required=True)
    build = sub.add_parser('build')
    for name in ('root', 'database', 'elf', 'app-prefix', 'output'):
        build.add_argument('--' + name, required=True)
    logs = sub.add_parser('logs')
    for name in ('native', 'target', 'output'):
        logs.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    try:
        result = (audit_build(args.root, args.database, args.elf, args.app_prefix)
                  if args.mode == 'build' else compare_logs(args.native.read_text(), args.target.read_text()))
        output = Path(args.output)
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(json.dumps(result, indent=2) + '\n')
        print(json.dumps(result, indent=2))
    except (ValueError, OSError, KeyError, TypeError) as exc:
        parser.exit(2, f'target evidence rejected: {exc}\n')


if __name__ == '__main__':
    main()
