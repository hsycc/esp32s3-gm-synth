#!/usr/bin/env python3
"""Collect a complete, relocatable qualification image; never flash or approve it."""
import argparse
import fnmatch
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import stat
import subprocess

from tools.v1_loop_realtime_manifest import PROFILES, PROFILES_V2, OUTPUT_MODES


SOURCE_PREFIXES = ('firmware/', 'main/', 'profiles/', 'scripts/', 'tools/',
                   'tests/', '.github/')
ROOT_SOURCE_PATTERNS = ('CMakeLists.txt', '*.cmake', 'sdkconfig*', 'partitions*.csv',
                        'dependencies.lock', 'idf_component.yml', '.gitattributes', '.gitignore')


def source_identity(root, owned_translation_units=()):
    """Record actual build inputs and dirty paths without copying unknown content."""
    root = Path(root).resolve()

    def git(*args):
        return subprocess.check_output(['git', '-C', str(root), *args])

    commit, tree = git('rev-parse', 'HEAD', 'HEAD^{tree}').decode().splitlines()
    object_format = git('rev-parse', '--show-object-format').decode().strip()
    status = git('status', '--porcelain=v1', '--untracked-files=all').decode()
    head = {}
    for entry in git('ls-tree', '-r', '-z', commit).split(b'\0'):
        if entry:
            metadata, path = entry.split(b'\t', 1)
            mode, kind, blob = metadata.decode().split()
            head[os.fsdecode(path)] = dict(mode=mode, kind=kind, blob=blob)
    tracked = {os.fsdecode(p) for p in git('ls-files', '-z').split(b'\0') if p}
    untracked = {os.fsdecode(p) for p in git('ls-files', '--others', '--exclude-standard', '-z').split(b'\0') if p}

    def in_scope(path):
        return path.startswith(SOURCE_PREFIXES) or ('/' not in path and any(
            fnmatch.fnmatchcase(path, pattern) for pattern in ROOT_SOURCE_PATTERNS))

    def fingerprint(relative):
        path = root / relative
        try:
            info = path.lstat()
        except FileNotFoundError:
            return dict(kind='missing')
        if stat.S_ISLNK(info.st_mode):
            # Hash the link itself. Never read or publish its target's contents.
            data = os.fsencode(os.readlink(path))
            blob = hashlib.new(object_format, b'blob ' + str(len(data)).encode() + b'\0' + data)
            return dict(kind='symlink', mode='120000', bytes=len(data),
                        sha256=hashlib.sha256(data).hexdigest(), git_blob=blob.hexdigest())
        if not path.resolve().is_relative_to(root):
            return dict(kind='outside_root', bytes=info.st_size)
        if not stat.S_ISREG(info.st_mode):
            return dict(kind='non_regular', bytes=info.st_size)
        sha256 = hashlib.sha256()
        blob = hashlib.new(object_format, b'blob ' + str(info.st_size).encode() + b'\0')
        with path.open('rb') as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b''):
                sha256.update(chunk)
                blob.update(chunk)
        return dict(kind='file', mode='100755' if info.st_mode & stat.S_IXUSR else '100644',
                    bytes=info.st_size, sha256=sha256.hexdigest(), git_blob=blob.hexdigest())

    owned = sorted(set(owned_translation_units))
    if any(Path(p).is_absolute() or '..' in Path(p).parts for p in owned):
        raise ValueError('owned translation unit is outside the source tree')
    paths = sorted({p for p in set(head) | tracked | untracked if in_scope(p)} | set(owned))
    rows = []
    for path in paths:
        expected = head.get(path)
        current = fingerprint(path)
        rows.append(dict(path=path, tracked=path in head or path in tracked,
                         head=expected, working=current,
                         matches_head=bool(expected and current.get('git_blob') == expected['blob']
                                           and current.get('mode') == expected['mode'])))
    by_path = {row['path']: row for row in rows}
    identity = dict(schema=1, source_commit=commit, source_tree=tree,
                    source_dirty=bool(status.strip()), git_object_format=object_format,
                    scope=dict(prefixes=SOURCE_PREFIXES, root_patterns=ROOT_SOURCE_PATTERNS,
                               soundfont_identity='asset-manifest.json and flash input SHA256'),
                    tracked_inputs_match_head=all(row['matches_head'] for row in rows if row['tracked']),
                    build_inputs_match_head=all(row['matches_head'] for row in rows),
                    owned_translation_units=owned,
                    owned_translation_units_match_head=all(by_path[p]['matches_head'] for p in owned),
                    inputs=rows,
                    untracked=[dict(path=p, **fingerprint(p)) for p in sorted(untracked)])
    return status, identity


def check_config(text, profile):
    if profile not in PROFILES:
        raise ValueError('unknown profile')
    config = {}
    for line in text.splitlines():
        if line.startswith('CONFIG_') and '=' in line:
            key, value = line.split('=', 1)
            # IDF 6.1 emits some compatibility aliases more than once with
            # the same value. Conflicting definitions remain ambiguous.
            if key in config and config[key] != value:
                raise ValueError('conflicting configuration key: ' + key)
            config[key] = value
    required = {
        'CONFIG_IDF_TARGET': '"esp32s3"',
        'CONFIG_ESPTOOLPY_FLASHSIZE_16MB': 'y',
        'CONFIG_ESPTOOLPY_FLASHMODE_DIO': 'y',
        'CONFIG_ESPTOOLPY_FLASHFREQ_80M': 'y',
        'CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240': 'y',
        'CONFIG_COMPILER_OPTIMIZATION_PERF': 'y',
        'CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE': 'y',
        'CONFIG_ESP_TASK_WDT_EN': 'y', 'CONFIG_ESP_TASK_WDT_INIT': 'y',
        'CONFIG_ESP_TASK_WDT_TIMEOUT_S': '5', 'CONFIG_ESP_TASK_WDT_PANIC': 'y',
        'CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0': 'y',
        'CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1': 'y',
        'CONFIG_GM_SYNTH_I2S_QUAL_MASTER_GAIN_Q15': '32767',
        'CONFIG_GM_SYNTH_I2S_QUAL_DAC_ATTENUATION_DB': '12',
        'CONFIG_GM_SYNTH_I2S_QUAL_DMA_DESC_NUM': '8',
        'CONFIG_GM_SYNTH_I2S_QUAL_DMA_FRAME_NUM': '64',
        'CONFIG_PARTITION_TABLE_CUSTOM': 'y',
        'CONFIG_PARTITION_TABLE_CUSTOM_FILENAME': '"profiles/v1-loop-realtime/partitions.csv"',
    }
    if profile == 'physical':
        required.update(CONFIG_SPIRAM='y', CONFIG_SPIRAM_MODE_OCT='y',
                        CONFIG_SPIRAM_SPEED_80M='y',
                        CONFIG_STDATOMIC_S32C1I_SPIRAM_WORKAROUND='y',
                        CONFIG_GM_SYNTH_PCM_ARENA_BYTES='4718592',
                        CONFIG_GM_SYNTH_I2S_QUAL_ENABLE='y')
    elif config.get('CONFIG_SPIRAM') == 'y' or config.get('CONFIG_GM_SYNTH_I2S_QUAL_ENABLE') == 'y':
        raise ValueError('QEMU must use its explicit internal-memory/non-I2S profile')
    for key, value in required.items():
        if config.get(key) != value:
            raise ValueError('configuration mismatch: ' + key)
    return dict(profile=profile, compared_settings=required,
                physical_timing_approved=False)


def flash_inputs(build, generated, description):
    """Read the exact generated flash mapping, reject omissions and overlaps."""
    build, generated = Path(build).resolve(), Path(generated).resolve()
    mapping = description.get('flash_files')
    if not isinstance(mapping, dict) or len(mapping) != 4:
        raise ValueError('expected bootloader, partition table, application and raw bank')
    expected = {0: 'bootloader.bin', 0x8000: 'partition-table.bin',
                0x10000: 'esp32s3_gm_synth.bin', 0x210000: 'bank.sf2'}
    entries = []
    for position, filename in mapping.items():
        offset = int(position, 0)
        path = Path(filename)
        path = path.resolve() if path.is_absolute() else (build / path).resolve()
        if not any(path.is_relative_to(root) for root in (build, generated)):
            raise ValueError('flash input outside this build/generated input')
        if offset not in expected or path.name != expected[offset] or not path.is_file():
            raise ValueError('unexpected or missing flash input')
        data = path.read_bytes()
        if not data or offset + len(data) > 0x1000000:
            raise ValueError('flash input exceeds N16 flash')
        entries.append(dict(offset=offset, path=path, bytes=len(data),
                            sha256=hashlib.sha256(data).hexdigest()))
    entries.sort(key=lambda row: row['offset'])
    if [row['offset'] for row in entries] != sorted(expected):
        raise ValueError('missing required flash offset')
    for left, right in zip(entries, entries[1:]):
        if left['offset'] + left['bytes'] > right['offset']:
            raise ValueError('overlapping flash inputs')
    return entries


def collect(root, build, generated, sdkconfig, output):
    root, build, generated = Path(root).resolve(), Path(build).resolve(), Path(generated).resolve()
    output, sdkconfig = Path(output), Path(sdkconfig)
    if output.exists():
        raise ValueError('refusing to overwrite existing flash bundle')
    spec = json.loads((generated / 'manifest.json').read_text())
    profile = spec['profile']
    expected = (PROFILES_V2 if spec.get('schema') == 2 else PROFILES
                if spec.get('schema') == 1 else {}).get(profile)
    if expected is None or any(spec.get(key) != value for key, value in expected.items()):
        raise ValueError('generated manifest does not match the pinned profile')
    if spec.get('schema') == 2 and (spec.get('output_mode') not in OUTPUT_MODES or
            spec.get('output_latency_frames') != (64 if spec['output_mode'] == 'peak' else 0)):
        raise ValueError('generated output mode/latency mismatch')
    config = check_config(sdkconfig.read_text(), profile)
    description = json.loads((build / 'flasher_args.json').read_text())
    entries = flash_inputs(build, generated, description)
    bank = entries[-1]
    if bank['sha256'] != spec['sha256'] or bank['bytes'] != spec['bytes']:
        raise ValueError('flash mapping bank differs from compiled manifest')
    write_args = description.get('write_flash_args')
    if not isinstance(write_args, list) or not all(isinstance(t, str) for t in write_args):
        raise ValueError('missing generated write_flash_args')
    normalized = [x.replace('_', '-') if x.startswith('--') else x for x in write_args]
    for flag, value in (('--flash-mode', 'dio'), ('--flash-freq', '80m'), ('--flash-size', '16MB')):
        if normalized.count(flag) != 1 or normalized[normalized.index(flag) + 1] != value:
            raise ValueError('unexpected flash parameters: ' + flag)
    if any(x in normalized for x in ('--encrypt', '--encrypt-files', '--force')):
        raise ValueError('unsupported flash mode')
    # Validate every required file before producing a usable-looking directory.
    required = [build / 'esp32s3_gm_synth.elf', build / 'esp32s3_gm_synth.map',
                build / 'compile_commands.json', build / 'project_description.json',
                build.parent / 'idf-version.txt',
                build.parent / 'build-audit.json', build.parent / 'atomic-backend-audit.json',
                generated / 'loop_realtime_manifest.h', generated / 'bank.sha256',
                root / 'profiles/v1-loop-realtime/partitions.csv']
    if any(not p.is_file() for p in required):
        raise ValueError('incomplete build/config/backend evidence')
    audit = json.loads((build.parent / 'build-audit.json').read_text())
    source_status, identity = source_identity(root, audit['translation_units'])
    output.mkdir(parents=True)
    (output / 'images').mkdir()
    flash_lines = [shlex.join(write_args)]
    flash_files = {}
    for row in entries:
        filename = f'images/{row["offset"]:06x}-{row["path"].name}'
        shutil.copyfile(row['path'], output / filename)
        row['file'] = filename
        del row['path']
        flash_lines.append(f'0x{row["offset"]:x} {filename}')
        flash_files[f'0x{row["offset"]:x}'] = filename
    (output / 'flash_args').write_text('\n'.join(flash_lines) + '\n')
    relocated = dict(description, flash_files=flash_files)
    # The canonical relocatable input is flash_args. SDK component details may
    # contain old paths, so do not present a mixed-path flasher_args.json.
    relocated = {k: relocated[k] for k in ('write_flash_args', 'flash_files', 'flash_settings', 'extra_esptool_args') if k in relocated}
    (output / 'flasher_args.json').write_text(json.dumps(relocated, indent=2) + '\n')
    for path in required:
        shutil.copyfile(path, output / path.name)
    shutil.copyfile(sdkconfig, output / 'sdkconfig')
    shutil.copyfile(generated / 'manifest.json', output / 'asset-manifest.json')
    shutil.copyfile(root / 'assets/sf2/SOURCES.md', output / 'SF2-SOURCES.md')
    if profile == 'physical':
        shutil.copyfile(root / 'assets/sf2/licenses/FM_GM_OpenSoundFont_GPL-3.0.txt', output / 'SF2-GPL-3.0.txt')
        shutil.copyfile(root / 'scripts/v1_loop_realtime_hardware.py', output / 'hardware.py')
        (output / 'HARDWARE.txt').write_text(
            'This bundle is a compiled qualification input, not a hardware or listening PASS.\n'
            'Use the pinned ESP-IDF v6.1 Python environment (esptool installed).\n'
            'Keep backups, serial logs and recordings OUTSIDE this immutable bundle.\n'
            'First inspect without connecting a device:\n'
            '  python hardware.py inspect\n'
            'Before the first layout transition, read all 16 MiB and preserve its SHA256:\n'
            '  python hardware.py backup --port PORT --output /path/to/original-backup --banner bb31908-dirty\n'
            'The --banner value is an operator observation; bb31908-dirty remains accepted.\n'
            'Flash always uses bootloader, partition table, application AND native SF2:\n'
            '  python hardware.py flash --port PORT --backup /path/to/original-backup --log /path/to/flash.log\n'
            'Capture the complete boot-to-return serial log for every on/off and bypass/peak run.\n'
            'Record the exact source tree, bundle hashes, device, power/codec setup and listening notes.\n'
            'The same original backup can be reused across the consolidated matrix.\n'
            'Rollback checks the same device MAC and full backup hash before restoring every byte:\n'
            '  python hardware.py rollback --port PORT --backup /path/to/original-backup --log /path/to/rollback.log\n'
            'No helper command approves timing, 24-Voice capacity, audio quality or V1 release.\n')
    (output / 'source-status.txt').write_text(source_status)
    (output / 'source-identity.json').write_text(json.dumps(identity, indent=2) + '\n')
    result = dict(schema=spec['schema'], source_commit=identity['source_commit'], source_tree=identity['source_tree'],
                  source_dirty=identity['source_dirty'],
                  tracked_inputs_match_head=identity['tracked_inputs_match_head'],
                  build_inputs_match_head=identity['build_inputs_match_head'],
                  owned_translation_units_match_head=identity['owned_translation_units_match_head'],
                  profile=profile, analysis=spec['analysis'],
                  runtime_approved=False, physical_timing_approved=False,
                  release_approved=False, flash_files=entries, configuration=config)
    if spec['schema'] == 2:
        result.update(output_mode=spec['output_mode'], output_latency_frames=spec['output_latency_frames'],
                      channels=spec['channels'], max_zones=spec['max_zones'], listening_approved=False)
    (output / 'bundle-manifest.json').write_text(json.dumps(result, indent=2) + '\n')
    hashes = []
    for path in sorted(output.rglob('*')):
        if path.is_file():
            hashes.append(hashlib.sha256(path.read_bytes()).hexdigest() + '  ' + path.relative_to(output).as_posix())
    (output / 'SHA256SUMS').write_text('\n'.join(hashes) + '\n')
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('root', 'build', 'generated', 'sdkconfig', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(collect(args.root, args.build, args.generated, args.sdkconfig, args.output), indent=2))
