#!/usr/bin/env python3
"""Explicit operator actions for a matching N16R8 qualification bundle.

No command scans ports. Inspect is entirely offline. Backup reads the complete
16 MiB Flash before any layout transition. Flash and rollback require that
backup, its hash, and the same observed device MAC. Nothing here approves
realtime performance, audio quality, or V1 release.

Command syntax follows Espressif's esptool documentation:
https://docs.espressif.com/projects/esptool/en/latest/esp32s3/esptool/basic-commands.html
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
from datetime import datetime, timezone

FLASH_BYTES = 0x1000000
BANK_SHA = '9650eb1c2202e2d92aa3be155e4e895cf6531a241c8ea413fc09e7f9159d4a62'
OFFSETS = (0, 0x8000, 0x10000, 0x210000)
NAMES = ('bootloader.bin', 'partition-table.bin', 'esp32s3_gm_synth.bin', 'bank.sf2')
MAC = re.compile(r'(?im)^\s*MAC:\s*([0-9a-f]{2}(?::[0-9a-f]{2}){5})\s*$')


def sha256(path):
    result = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            result.update(block)
    return result.hexdigest()


def local_file(directory, relative):
    relative = Path(relative)
    if relative.is_absolute() or '..' in relative.parts or not relative.parts:
        raise ValueError('unsafe relative bundle/backup path')
    path = directory / relative
    if (path.is_symlink() or not path.is_file() or
            not path.resolve().is_relative_to(directory.resolve())):
        raise ValueError('missing, linked, or external bundle/backup file: ' + str(relative))
    return path


def inspect_bundle(directory):
    directory = Path(directory).resolve()
    sums = local_file(directory, 'SHA256SUMS').read_text().splitlines()
    verified = {}
    for line in sums:
        match = re.fullmatch(r'([0-9a-f]{64})  (.+)', line)
        if not match or match[2] in verified or match[2] == 'SHA256SUMS':
            raise ValueError('invalid or duplicate SHA256SUMS entry')
        path = local_file(directory, match[2])
        if sha256(path) != match[1]:
            raise ValueError('bundle SHA256 mismatch: ' + match[2])
        verified[match[2]] = match[1]
    actual = {p.relative_to(directory).as_posix() for p in directory.rglob('*')
              if p.is_file() and p.name != 'SHA256SUMS'}
    if set(verified) != actual:
        raise ValueError('bundle file list differs from SHA256SUMS')
    manifest = json.loads(local_file(directory, 'bundle-manifest.json').read_text())
    asset = json.loads(local_file(directory, 'asset-manifest.json').read_text())
    if (manifest.get('profile') != 'physical' or asset.get('profile') != 'physical' or
            asset.get('sha256') != BANK_SHA or asset.get('bytes') != 5898214 or
            asset.get('arena_bytes') != 4718592 or
            any(manifest.get(k) is not False for k in
                ('runtime_approved', 'physical_timing_approved', 'release_approved'))):
        raise ValueError('not the pinned physical qualification bundle')
    if any(manifest.get(k) is not True for k in
           ('tracked_inputs_match_head', 'build_inputs_match_head', 'owned_translation_units_match_head')):
        raise ValueError('actual build input identity does not match the recorded source tree')
    files = manifest.get('flash_files')
    if not isinstance(files, list) or len(files) != 4:
        raise ValueError('complete four-input Flash layout required')
    previous_end = 0
    for row, offset, name in zip(files, OFFSETS, NAMES):
        expected_name = f'images/{offset:06x}-{name}'
        if row.get('offset') != offset or row.get('file') != expected_name:
            raise ValueError('unexpected Flash offset or image name')
        path = local_file(directory, expected_name)
        size = path.stat().st_size
        if (size != row.get('bytes') or size <= 0 or offset < previous_end or
                offset + size > FLASH_BYTES or sha256(path) != row.get('sha256') or
                verified.get(expected_name) != row.get('sha256')):
            raise ValueError('Flash input hash/size/range mismatch')
        previous_end = offset + size
    if files[-1]['sha256'] != BANK_SHA or files[-1]['bytes'] != 5898214:
        raise ValueError('Flash bank differs from the pinned native SF2')
    return manifest


def check_backup(directory):
    directory = Path(directory).resolve()
    manifest = json.loads(local_file(directory, 'backup.json').read_text())
    image = local_file(directory, 'flash-16MiB.bin')
    if (manifest.get('schema') != 1 or manifest.get('chip') != 'esp32s3' or
            manifest.get('bytes') != FLASH_BYTES or image.stat().st_size != FLASH_BYTES or
            not re.fullmatch(r'[0-9a-f]{2}(?::[0-9a-f]{2}){5}', manifest.get('mac', '')) or
            sha256(image) != manifest.get('sha256')):
        raise ValueError('complete, hash-matching N16R8 backup required')
    return manifest


def run_esptool(port, arguments, log):
    # An argv vector deliberately avoids shell interpolation and permits no
    # caller-supplied force/encryption/erase-all flags.
    command = [sys.executable, '-m', 'esptool', '--chip', 'esp32s3', '--port', port,
               '--baud', '460800', *arguments]
    completed = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               text=True, timeout=900, check=False)
    log.write(completed.stdout)
    log.flush()
    print(completed.stdout, end='')
    if completed.returncode:
        raise RuntimeError('esptool failed; preserve its log and the backup')
    return completed.stdout


def device_mac(port, log):
    values = {m.lower() for m in MAC.findall(run_esptool(port, ['read-mac'], log))}
    if len(values) != 1:
        raise ValueError('cannot establish one device MAC from esptool output')
    return values.pop()


def backup(port, directory, banner='unrecorded'):
    directory = Path(directory).resolve()
    directory.mkdir(parents=True, exist_ok=False)
    image = directory / 'flash-16MiB.bin'
    with (directory / 'esptool.log').open('x') as log:
        mac = device_mac(port, log)
        run_esptool(port, ['read-flash', '0x0', hex(FLASH_BYTES), str(image)], log)
    if image.stat().st_size != FLASH_BYTES:
        raise ValueError('partial Flash backup; do not flash using this directory')
    result = dict(schema=1, chip='esp32s3', bytes=FLASH_BYTES, mac=mac,
                  sha256=sha256(image), operator_reported_banner=banner,
                  created_utc=datetime.now(timezone.utc).isoformat(),
                  baseline_reference='bb31908b; bb31908-dirty was accepted, not requalified here',
                  physical_timing_approved=False, listening_approved=False)
    (directory / 'backup.json').write_text(json.dumps(result, indent=2) + '\n')
    (directory / 'SHA256SUMS').write_text(result['sha256'] + '  flash-16MiB.bin\n')
    check_backup(directory)
    return result


def flash_or_rollback(port, backup_directory, log_path, bundle=None):
    saved = check_backup(backup_directory)
    # Validate everything before even opening a serial connection.
    manifest = inspect_bundle(bundle) if bundle is not None else None
    log_path = Path(log_path).resolve()
    if bundle is not None and log_path.is_relative_to(Path(bundle).resolve()):
        raise ValueError('keep runtime logs outside the immutable Flash bundle')
    with log_path.open('x') as log:
        if device_mac(port, log) != saved['mac']:
            raise ValueError('backup belongs to another device; no Flash write performed')
        if manifest is None:
            arguments = ['write-flash', '--flash-mode', 'keep', '--flash-freq', 'keep',
                         '--flash-size', 'keep', '0x0',
                         str(Path(backup_directory).resolve() / 'flash-16MiB.bin')]
        else:
            arguments = ['write-flash', '--flash-mode', 'dio', '--flash-freq', '80m',
                         '--flash-size', '16MB']
            for row in manifest['flash_files']:
                arguments.extend([hex(row['offset']), str(Path(bundle).resolve() / row['file'])])
        run_esptool(port, arguments, log)
    return dict(action='rollback' if manifest is None else 'flash', mac=saved['mac'],
                source_commit=None if manifest is None else manifest['source_commit'],
                backup_sha256=saved['sha256'], physical_timing_approved=False,
                listening_approved=False, release_approved=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    inspect = commands.add_parser('inspect', help='offline SHA256 and complete image validation')
    inspect.add_argument('--bundle', type=Path, default=Path(__file__).resolve().parent)
    save = commands.add_parser('backup', help='read all 16 MiB before changing the layout')
    save.add_argument('--port', required=True)
    save.add_argument('--output', type=Path, required=True)
    save.add_argument('--banner', default='unrecorded', help='operator-observed boot banner, never auto-approved')
    for name in ('flash', 'rollback'):
        command = commands.add_parser(name)
        command.add_argument('--port', required=True)
        command.add_argument('--backup', type=Path, required=True)
        command.add_argument('--log', type=Path, required=True)
        if name == 'flash':
            command.add_argument('--bundle', type=Path, default=Path(__file__).resolve().parent)
    args = parser.parse_args()
    try:
        if args.command == 'inspect':
            result = inspect_bundle(args.bundle)
        elif args.command == 'backup':
            result = backup(args.port, args.output, args.banner)
        else:
            result = flash_or_rollback(args.port, args.backup, args.log, getattr(args, 'bundle', None))
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as error:
        parser.exit(2, f'hardware action rejected: {error}\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
