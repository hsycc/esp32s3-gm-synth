#!/usr/bin/env python3
"""Collect a complete, relocatable qualification image; never flash or approve it."""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import shutil
import subprocess

from tools.v1_loop_realtime_manifest import PROFILES


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
    expected = PROFILES.get(profile)
    if expected is None or any(spec.get(key) != value for key, value in expected.items()):
        raise ValueError('generated manifest does not match the pinned profile')
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
    source_commit = subprocess.check_output(['git', '-C', str(root), 'rev-parse', 'HEAD'], text=True).strip()
    source_tree = subprocess.check_output(['git', '-C', str(root), 'rev-parse', 'HEAD^{tree}'], text=True).strip()
    dirty = bool(subprocess.check_output(['git', '-C', str(root), 'status', '--porcelain', '--untracked-files=normal'], text=True).strip())
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
    result = dict(schema=1, source_commit=source_commit, source_tree=source_tree,
                  source_dirty=dirty, profile=profile, analysis=spec['analysis'],
                  runtime_approved=False, physical_timing_approved=False,
                  release_approved=False, flash_files=entries, configuration=config)
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
