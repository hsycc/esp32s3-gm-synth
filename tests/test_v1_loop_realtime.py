import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock

from tests.app.make_loop_replay_fixture import generate as fixture
from tests.app.make_loop_analysis_fixture import generate as legacy_fixture
from tools.v1_loop_realtime_manifest import generate, PROFILES, PROFILES_V2, SYNTHETIC_SHA, REPLAY_SYNTHETIC_SHA, FM_SHA
from tools.v1_loop_realtime_bundle import check_config, flash_inputs, source_identity
from tools.v1_loop_realtime_evidence import validate, validate_host, validate_output_host, replay_hash
from scripts import v1_loop_realtime_hardware as hardware

ROOT = Path(__file__).resolve().parents[1]


class Manifest(unittest.TestCase):
    def test_fixture_is_explicit_and_off_by_default(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            fixture(directory / 'fixture')
            source = directory / 'fixture/fixture.sf2'
            self.assertEqual(generate(source, directory / 'out', 'qemu'), REPLAY_SYNTHETIC_SHA)
            header = (directory / 'out/loop_realtime_manifest.h').read_text()
            self.assertIn('analysis=false', header)
            self.assertIn('output_peak=false', header)
            self.assertIn('output_mode[]="bypass"', header)
            manifest = json.loads((directory / 'out/manifest.json').read_text())
            self.assertEqual((manifest['schema'], manifest['channels'], manifest['max_zones']), (2, 2, 16))
            self.assertEqual((manifest['blocks'], manifest['output_latency_frames']), (6144, 0))
            self.assertIn('synthetic=true', header)
            self.assertEqual((directory / 'out/bank.sf2').read_bytes(), source.read_bytes())

    def test_real_bank_reuses_original_bytes_and_selects_a_looping_program(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            source = ROOT / 'assets/sf2/subsets/FM_GM_Open_subset_6MiB.sf2'
            self.assertEqual(generate(source, directory, 'physical', True), FM_SHA)
            manifest = json.loads((directory / 'manifest.json').read_text())
            self.assertEqual(manifest['bytes'], 5898214)
            self.assertEqual(manifest['arena_bytes'], 4718592)
            self.assertEqual(manifest['initial_program'], 16)
            self.assertEqual(manifest['replacement_program'], 1)
            self.assertTrue(manifest['analysis'])
            self.assertFalse(manifest['synthetic'])
            self.assertFalse(manifest['physical_timing_approved'])
            self.assertEqual(hashlib.sha256((directory / 'bank.sf2').read_bytes()).hexdigest(), FM_SHA)

    def test_peak_mode_is_explicit_and_cannot_change_workload(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            fixture(directory / 'fixture')
            source = directory / 'fixture/fixture.sf2'
            generate(source, directory / 'out', 'qemu', True, 'peak')
            report = json.loads((directory / 'out/manifest.json').read_text())
            self.assertEqual(report['output_mode'], 'peak')
            self.assertEqual(report['output_latency_frames'], 64)
            self.assertTrue(report['analysis'])
            for key, value in PROFILES_V2['qemu'].items():
                self.assertEqual(report[key], value)
            self.assertEqual(PROFILES['qemu']['blocks'], 2048)
            self.assertEqual(PROFILES['qemu']['sha256'], SYNTHETIC_SHA)
            with self.assertRaises(ValueError):
                generate(source, directory / 'bad', 'qemu', True, 'magic-auto-gain')
            legacy_fixture(directory / 'legacy')
            with self.assertRaises(ValueError):
                generate(directory / 'legacy/fixture.sf2', directory / 'bad-bank', 'qemu', True, 'peak')

    def test_substitution_and_changed_bytes_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            fixture(directory / 'fixture')
            source = directory / 'fixture/fixture.sf2'
            with self.assertRaises(ValueError):
                generate(source, directory / 'out', 'physical', True)
            raw = bytearray(source.read_bytes())
            raw[-1] ^= 1
            source.write_bytes(raw)
            with self.assertRaises(ValueError):
                generate(source, directory / 'out', 'qemu', True)

    def test_corrupt_container_and_unknown_profile_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            source = directory / 'bad.sf2'
            source.write_bytes(b'RIFF')
            for profile in ('qemu', 'physical', 'unknown'):
                with self.subTest(profile=profile), self.assertRaises(ValueError):
                    generate(source, directory / 'out', profile)


class Configuration(unittest.TestCase):
    def config(self, profile):
        directory = ROOT / 'profiles/v1-loop-realtime'
        text = (directory / 'sdkconfig.common').read_text() + (directory / ('sdkconfig.' + profile)).read_text()
        if profile == 'physical':
            text += 'CONFIG_STDATOMIC_S32C1I_SPIRAM_WORKAROUND=y\n'
        return text

    def test_explicit_profiles(self):
        for profile in PROFILES:
            with self.subTest(profile=profile):
                self.assertFalse(check_config(self.config(profile), profile)['physical_timing_approved'])

    def test_sdk_compatibility_aliases_may_repeat_identically(self):
        # IDF 6.1 generates this legacy Wi-Fi alias twice in sdkconfig.
        for profile in PROFILES:
            with self.subTest(profile=profile):
                config = self.config(profile)
                alias = 'CONFIG_ESP32_WIFI_STATIC_RX_BUFFER_NUM=10\n'
                expected = check_config(config + alias, profile)
                self.assertEqual(check_config(config + alias + alias, profile), expected)
                self.assertEqual(check_config(config + alias + alias +
                    'CONFIG_GM_SYNTH_I2S_QUAL_MASTER_GAIN_Q15=32767\n', profile), expected)

    def test_conflicting_duplicate_configuration_is_rejected(self):
        config = self.config('physical')
        for additions in (
            'CONFIG_ESP32_WIFI_STATIC_RX_BUFFER_NUM=10\n'
            'CONFIG_ESP32_WIFI_STATIC_RX_BUFFER_NUM=32\n',
            'CONFIG_GM_SYNTH_I2S_QUAL_MASTER_GAIN_Q15=16384\n',
        ):
            with self.subTest(additions=additions), self.assertRaisesRegex(ValueError, 'conflicting configuration key'):
                check_config(config + additions, 'physical')

    def test_no_hidden_capacity_buffer_volume_or_watchdog_relaxation(self):
        config = self.config('physical')
        for original, replacement in (
            ('CONFIG_GM_SYNTH_PCM_ARENA_BYTES=4718592', 'CONFIG_GM_SYNTH_PCM_ARENA_BYTES=2097152'),
            ('CONFIG_GM_SYNTH_I2S_QUAL_DMA_DESC_NUM=8', 'CONFIG_GM_SYNTH_I2S_QUAL_DMA_DESC_NUM=16'),
            ('CONFIG_GM_SYNTH_I2S_QUAL_DMA_FRAME_NUM=64', 'CONFIG_GM_SYNTH_I2S_QUAL_DMA_FRAME_NUM=128'),
            ('CONFIG_GM_SYNTH_I2S_QUAL_MASTER_GAIN_Q15=32767', 'CONFIG_GM_SYNTH_I2S_QUAL_MASTER_GAIN_Q15=16384'),
            ('CONFIG_GM_SYNTH_I2S_QUAL_DAC_ATTENUATION_DB=12', 'CONFIG_GM_SYNTH_I2S_QUAL_DAC_ATTENUATION_DB=6'),
            ('CONFIG_STDATOMIC_S32C1I_SPIRAM_WORKAROUND=y', 'CONFIG_STDATOMIC_S32C1I_SPIRAM_WORKAROUND=n'),
            ('CONFIG_ESP_TASK_WDT_PANIC=y', 'CONFIG_ESP_TASK_WDT_PANIC=n'),
            ('CONFIG_ESP_TASK_WDT_TIMEOUT_S=5', 'CONFIG_ESP_TASK_WDT_TIMEOUT_S=10'),
            ('CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1=y', 'CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1=n'),
        ):
            with self.subTest(setting=original), self.assertRaises(ValueError):
                check_config(config.replace(original, replacement), 'physical')

    def test_qemu_not_a_physical_profile(self):
        with self.assertRaises(ValueError):
            check_config(self.config('qemu'), 'physical')
        with self.assertRaises(ValueError):
            check_config(self.config('physical'), 'qemu')


class SourceIdentity(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name) / 'source'
        self.root.mkdir()
        for path in ('firmware/a.cpp', 'firmware/include/a.h', 'main/app.cpp',
                     'profiles/qualification/sdkconfig', 'CMakeLists.txt',
                     'sdkconfig.defaults', 'partitions.csv', 'assets/bank.sf2'):
            target = self.root / path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text('original\n')
        self.git('init', '-q')
        self.git('add', '.')
        self.git('-c', 'user.name=Identity test', '-c', 'user.email=identity@example.invalid',
                 'commit', '-qm', 'fixture')

    def git(self, *args):
        return subprocess.check_output(['git', '-C', str(self.root), *args])

    def test_clean_inputs_cover_headers_configuration_and_owned_units(self):
        status, result = source_identity(self.root, ['firmware/a.cpp', 'main/app.cpp'])
        self.assertEqual(status, '')
        self.assertFalse(result['source_dirty'])
        self.assertTrue(result['tracked_inputs_match_head'])
        self.assertTrue(result['build_inputs_match_head'])
        self.assertTrue(result['owned_translation_units_match_head'])
        paths = {row['path'] for row in result['inputs']}
        self.assertTrue({'firmware/include/a.h', 'profiles/qualification/sdkconfig',
                         'CMakeLists.txt', 'sdkconfig.defaults', 'partitions.csv'} <= paths)
        self.assertNotIn('assets/bank.sf2', paths)

    def test_index_dirty_and_working_input_identity_are_separate(self):
        source = self.root / 'firmware/a.cpp'
        source.write_text('staged change\n')
        self.git('add', 'firmware/a.cpp')
        source.write_text('original\n')
        status, result = source_identity(self.root, ['firmware/a.cpp'])
        self.assertIn('MM firmware/a.cpp', status)
        self.assertTrue(result['source_dirty'])
        self.assertTrue(result['tracked_inputs_match_head'])
        source.write_text('actual changed bytes\n')
        _, result = source_identity(self.root, ['firmware/a.cpp'])
        self.assertFalse(result['tracked_inputs_match_head'])
        self.assertFalse(result['owned_translation_units_match_head'])

    def test_staged_rename_retains_deleted_and_added_input(self):
        self.git('mv', 'firmware/a.cpp', 'firmware/new name.cpp')
        _, result = source_identity(self.root, ['firmware/new name.cpp'])
        rows = {row['path']: row for row in result['inputs']}
        self.assertEqual(rows['firmware/a.cpp']['working']['kind'], 'missing')
        self.assertIsNone(rows['firmware/new name.cpp']['head'])
        self.assertFalse(result['tracked_inputs_match_head'])
        self.assertFalse(result['owned_translation_units_match_head'])

    def test_untracked_config_records_identity_and_symlink_does_not_follow_target(self):
        value = 'private configuration content is not an artifact\n'
        (self.root / 'config.toml').write_text(value)
        nested = self.root / 'scratch/nested config.toml'
        nested.parent.mkdir()
        nested.write_text(value)
        outside = Path(self.temporary.name) / 'outside-secret'
        outside.write_text('target content must not be read\n')
        (self.root / 'external-link').symlink_to(outside)
        status, result = source_identity(self.root, ['firmware/a.cpp'])
        self.assertIn('scratch/nested config.toml', status)
        self.assertTrue(result['source_dirty'])
        self.assertTrue(result['tracked_inputs_match_head'])
        self.assertTrue(result['build_inputs_match_head'])
        rows = {row['path']: row for row in result['untracked']}
        self.assertEqual(rows['config.toml']['sha256'], hashlib.sha256(value.encode()).hexdigest())
        self.assertEqual(rows['external-link']['kind'], 'symlink')
        self.assertEqual(rows['external-link']['sha256'], hashlib.sha256(os.fsencode(outside)).hexdigest())
        self.assertNotIn(value, json.dumps(result))
        self.assertNotIn(str(outside), json.dumps(result))


class EarlyDependencyScan(unittest.TestCase):
    def scan(self, mode, startup='OFF'):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            generated = directory / 'generated'
            generated.mkdir()
            for name in ('loop_realtime_manifest.h', 'loop_startup_manifest.h', 'bank.sf2'):
                (generated / name).touch()
            child = directory / 'early.cmake'
            child.write_text('function(idf_component_register)\n'
                ' file(WRITE "' + (directory / 'requirements.txt').as_posix() + '" "${ARGV}")\n'
                'endfunction()\ninclude("' + (ROOT / 'main/CMakeLists.txt').as_posix() + '")\n')
            prefix = (ROOT / 'CMakeLists.txt').read_text().split('set(EXTRA_COMPONENT_DIRS', 1)[0]
            (directory / 'CMakeLists.txt').write_text(prefix + '\nproject(scan NONE)\n'
                'execute_process(COMMAND "${CMAKE_COMMAND}" -P "' + child.as_posix() + '" RESULT_VARIABLE result)\n'
                'if(NOT result EQUAL 0)\nmessage(FATAL_ERROR "early child failed")\nendif()\n')
            env = dict(os.environ, GM_SYNTH_REALTIME_EARLY_MODE='1',
                       GM_SYNTH_REALTIME_EARLY_GENERATED='/invalid/inherited/path',
                       GM_SYNTH_STARTUP_EARLY_MODE='1', GM_SYNTH_STARTUP_EARLY_GENERATED='/invalid/startup/path')
            subprocess.run(['cmake', '-S', str(directory), '-B', str(directory / 'build'),
                '-DGM_SYNTH_BUILD_LOOP_REALTIME_QUAL=' + mode,
                '-DGM_SYNTH_REALTIME_GENERATED=' + str(generated),
                '-DGM_SYNTH_BUILD_LOOP_STARTUP_QUAL=' + startup,
                '-DGM_SYNTH_STARTUP_GENERATED=' + str(generated)],
                env=env, check=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            return (directory / 'requirements.txt').read_text()

    def test_enabled_dependencies_reach_fresh_child(self):
        for mode in ('ON', 'TRUE', '1'):
            with self.subTest(mode=mode):
                args = self.scan(mode)
                for needed in ('LoopRealtimeQualification.cpp', 'LoopRealtimeI2s.cpp',
                               'KorvoV4Es8311.cpp', 'mbedtls', 'esp_driver_i2s', 'esp_driver_i2c'):
                    self.assertIn(needed, args)
                self.assertNotIn('app_main.cpp', args)
                self.assertNotIn('LoopStartupQualification.cpp', args)

    def test_disabled_clears_caller_environment(self):
        args = self.scan('OFF')
        self.assertIn('app_main.cpp', args)
        self.assertNotIn('LoopRealtimeQualification.cpp', args)
        self.assertNotIn('mbedtls', args)
        self.assertNotIn('/invalid/', args)

    def test_serial_startup_is_still_independent(self):
        args = self.scan('OFF', 'ON')
        self.assertIn('LoopStartupQualification.cpp', args)
        self.assertNotIn('LoopRealtimeQualification.cpp', args)

    def test_entries_cannot_be_combined(self):
        with self.assertRaises(subprocess.CalledProcessError):
            self.scan('ON', 'ON')


class FlashBundle(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.build = self.root / 'target'
        self.generated = self.root / 'generated'
        self.build.mkdir()
        self.generated.mkdir()
        self.files = {}
        for offset, name in ((0, 'bootloader.bin'), (0x8000, 'partition-table.bin'),
                             (0x10000, 'esp32s3_gm_synth.bin'), (0x210000, 'bank.sf2')):
            path = (self.generated if name == 'bank.sf2' else self.build) / name
            path.write_bytes(b'fixture binary')
            self.files[hex(offset)] = str(path)

    def test_exact_flash_map(self):
        rows = flash_inputs(self.build, self.generated, {'flash_files': self.files})
        self.assertEqual([r['offset'] for r in rows], [0, 0x8000, 0x10000, 0x210000])
        self.assertEqual(len({r['sha256'] for r in rows}), 1)

    def test_app_only_image_is_rejected(self):
        for key in self.files:
            files = dict(self.files)
            del files[key]
            with self.subTest(missing=key), self.assertRaises(ValueError):
                flash_inputs(self.build, self.generated, {'flash_files': files})

    def test_wrong_partition_or_overlap_rejected(self):
        files = dict(self.files)
        files['0x220000'] = files.pop('0x210000')
        with self.assertRaises(ValueError):
            flash_inputs(self.build, self.generated, {'flash_files': files})
        (self.build / 'bootloader.bin').write_bytes(bytes(0x8001))
        with self.assertRaises(ValueError):
            flash_inputs(self.build, self.generated, {'flash_files': self.files})


    def test_external_file_and_missing_bytes_rejected(self):
        outside = self.root / 'bank.sf2'
        outside.write_bytes(b'not the generated input')
        files = dict(self.files, **{'0x210000': str(outside)})
        with self.assertRaises(ValueError):
            flash_inputs(self.build, self.generated, {'flash_files': files})
        (self.generated / 'bank.sf2').unlink()
        with self.assertRaises(ValueError):
            flash_inputs(self.build, self.generated, {'flash_files': self.files})


class HardwareOperations(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.bundle = self.root / 'bundle'
        (self.bundle / 'images').mkdir(parents=True)
        rows = []
        for offset, name in zip(hardware.OFFSETS, hardware.NAMES):
            relative = f'images/{offset:06x}-{name}'
            data = (ROOT / 'assets/sf2/subsets/FM_GM_Open_subset_6MiB.sf2').read_bytes() \
                if name == 'bank.sf2' else b'qualification fixture'
            (self.bundle / relative).write_bytes(data)
            rows.append(dict(offset=offset, file=relative, bytes=len(data),
                             sha256=hashlib.sha256(data).hexdigest()))
        self.manifest = dict(schema=2, profile='physical', source_commit='a' * 40,
            runtime_approved=False, physical_timing_approved=False, release_approved=False,
            tracked_inputs_match_head=True, build_inputs_match_head=True,
            owned_translation_units_match_head=True, flash_files=rows)
        (self.bundle / 'bundle-manifest.json').write_text(json.dumps(self.manifest))
        (self.bundle / 'asset-manifest.json').write_text(json.dumps(
            dict(PROFILES_V2['physical'], schema=2, profile='physical', output_mode='peak')))
        self.resign()
        self.backup = self.root / 'backup'

    def resign(self):
        lines = [hardware.sha256(p) + '  ' + p.relative_to(self.bundle).as_posix()
                 for p in sorted(self.bundle.rglob('*')) if p.is_file() and p.name != 'SHA256SUMS']
        (self.bundle / 'SHA256SUMS').write_text('\n'.join(lines) + '\n')

    def save_backup(self):
        def fake(port, arguments, log):
            self.assertEqual(port, 'test-port')
            if arguments == ['read-mac']:
                return 'MAC: 12:34:56:78:9a:bc\n'
            self.assertEqual(arguments[:3], ['read-flash', '0x0', '0x1000000'])
            Path(arguments[3]).write_bytes(bytes(hardware.FLASH_BYTES))
            return 'read complete\n'
        with mock.patch.object(hardware, 'run_esptool', side_effect=fake):
            hardware.backup('test-port', self.backup, 'bb31908-dirty')

    def test_offline_inspection_detects_tampering_and_app_only_bundle(self):
        with mock.patch.object(hardware, 'run_esptool', side_effect=AssertionError('no serial access')):
            self.assertEqual(hardware.inspect_bundle(self.bundle)['source_commit'], 'a' * 40)
            path = self.bundle / self.manifest['flash_files'][2]['file']
            path.write_bytes(b'tampered')
            with self.assertRaisesRegex(ValueError, 'SHA256 mismatch'):
                hardware.inspect_bundle(self.bundle)
            self.resign()
            with self.assertRaisesRegex(ValueError, 'Flash input hash'):
                hardware.inspect_bundle(self.bundle)
            self.manifest['flash_files'] = self.manifest['flash_files'][2:3]
            (self.bundle / 'bundle-manifest.json').write_text(json.dumps(self.manifest))
            self.resign()
            with self.assertRaisesRegex(ValueError, 'four-input'):
                hardware.inspect_bundle(self.bundle)

    def test_full_backup_and_same_device_required_before_write(self):
        self.save_backup()
        saved = hardware.check_backup(self.backup)
        self.assertEqual(saved['operator_reported_banner'], 'bb31908-dirty')
        with self.assertRaises(FileExistsError):
            hardware.backup('test-port', self.backup)
        with mock.patch.object(hardware, 'device_mac', return_value='00:00:00:00:00:00'), \
                mock.patch.object(hardware, 'run_esptool') as command:
            with self.assertRaisesRegex(ValueError, 'another device'):
                hardware.flash_or_rollback('test-port', self.backup, self.root / 'wrong-device.log', self.bundle)
            command.assert_not_called()
        (self.backup / 'flash-16MiB.bin').write_bytes(b'partial')
        with mock.patch.object(hardware, 'device_mac') as identify:
            with self.assertRaisesRegex(ValueError, 'backup required'):
                hardware.flash_or_rollback('test-port', self.backup, self.root / 'partial.log', self.bundle)
            identify.assert_not_called()

    def test_flash_uses_all_four_inputs_and_rollback_preserves_complete_image(self):
        self.save_backup()
        with mock.patch.object(hardware, 'device_mac', return_value='12:34:56:78:9a:bc'), \
                mock.patch.object(hardware, 'run_esptool', return_value='verified') as command:
            result = hardware.flash_or_rollback('test-port', self.backup, self.root / 'flash.log', self.bundle)
            arguments = command.call_args.args[1]
            self.assertEqual(arguments[:7], ['write-flash', '--flash-mode', 'dio', '--flash-freq',
                                            '80m', '--flash-size', '16MB'])
            self.assertEqual(arguments[7::2], ['0x0', '0x8000', '0x10000', '0x210000'])
            self.assertFalse(result['physical_timing_approved'])
            self.assertFalse(result['listening_approved'])
            hardware.flash_or_rollback('test-port', self.backup, self.root / 'rollback.log')
            arguments = command.call_args.args[1]
            self.assertEqual(arguments, ['write-flash', '--flash-mode', 'keep', '--flash-freq', 'keep',
                                        '--flash-size', 'keep', '0x0', str(self.backup / 'flash-16MiB.bin')])

NATIVE = '''REALTIME_HOST_CASE default_off PASS
REALTIME_HOST_CASE continuous_real_provider overlapped_blocks=24 midi_events=11 PASS
REALTIME_HOST_CASE missing_program_keeps_audio PASS
REALTIME_HOST_CASE residency_failure_keeps_audio PASS
REALTIME_HOST_CASE stop_ready_before_commit PASS
REALTIME_HOST_CASE stop_committed_before_ack PASS
REALTIME_HOST_CASE stop_during_real_analysis stopped_actors=2 control_joined=1 PASS
REALTIME_HOST_CASE cancelled_analysis_direct_fallback PASS
REALTIME_HOST_CASE storage_exclusive_then_reusable PASS
REALTIME_BANK_RESULT mode=off voices=24 loops=0 clipped=9950 headroom_pass=0 physical_timing_approved=0
REALTIME_BANK_RESULT mode=on voices=24 loops=24 clipped=4479 headroom_pass=0 physical_timing_approved=0
REALTIME_HOST_CASE real_fm_cohort_headroom_rejected PASS physical_timing_approved=0
REALTIME_HOST_RESULT suites=10 checks=100 PASS actor_ownership=1
'''
TARGET = f'''REALTIME_BEGIN profile=qemu analysis=1 synthetic=1 root_application=1 control_core=0 audio_core=1
REALTIME_MEMORY phase=before internal_free=300000 internal_largest=280000 internal_min=290000 psram_free=0 psram_largest=0
REALTIME_READY status=1 session=1 plans=3 analyses=3 session_bytes=170000 storage_bytes=100000 storage_psram=0 arena_capacity=20480
REALTIME_ASSET bytes=25866 sha256={SYNTHETIC_SHA} initial_program=0 replacement_program=1
REALTIME_MEMORY phase=ready internal_free=30000 internal_largest=20000 internal_min=29000 psram_free=0 psram_largest=0
REALTIME_ACTORS control_core=0 audio_core=1
REALTIME_GATES integration=PASS cohort=NOT_RUN digital_headroom=PASS
REALTIME_RESULT frames=131072 blocks=2048 checksum=123 events=19 rejected=0 clipped=0 voices_peak=4 loop_voices_peak=2 program_switches=2 blocks_during_prepare=20
REALTIME_CONTROL queued=19 note_on=4 note_off=3 cc=10 sustain=4 program_change=2 services=300 prepared=2 acknowledgements=2 ack_failures=0 io_reads=99 io_bytes=30000 io_yields=2 analysis_yields=30
REALTIME_TIMING startup_us=100 hash_us=20 prepare_avg_us=50 prepare_max_us=80 render_avg_us=60 render_p95_upper_us=95 render_p99_upper_us=111 render_max_us=110 quantile_bucket_us=16 physical=0
REALTIME_STAGES read_us=100 read_max_us=30 analysis_us=1000 analysis_max_us=400 analysis_calls=7 read_excludes_yield=1 analysis_includes_yield=1
REALTIME_HEALTH render_soft_overruns=0 work_soft_overruns=0 max_backlog_us=0 final_backlog_us=0 min_dma_margin_us=11610 exceeded=0 stalls=0 writes=0 write_errors=0 timeouts=0 partial=0 sent=0 dma_queue_overflows=0 event_queue_drops=0 audio_stack=4000 control_stack=10000
REALTIME_COHORT requested=24 required=0 blocks=0 voices_min=0 voices_max=0 loops_min=0 loops_max=0 clipped=0 render_avg_us=0 render_p95_upper_us=0 render_p99_upper_us=0 render_max_us=0 max_backlog_us=0 final_backlog_us=0 gate=NOT_RUN
REALTIME_PCM capacity=20480 high_water=16640 used=16640 allocation_failures=0 stale_refs=0
REALTIME_MEMORY phase=joined internal_free=30000 internal_largest=20000 internal_min=27000 psram_free=0 psram_largest=0
REALTIME_STOP started=1 audio_stopped=1 control_stopped=1 releases=1 mapped=0 arena_live=0 live_refs=0 acquirers=0 stale_refs=0
REALTIME_MEMORY phase=stopped internal_free=300000 internal_largest=280000 internal_min=27000 psram_free=0 psram_largest=0
REALTIME_PASS runtime_approved=0 physical_timing_approved=0
I (123) main_task: Returned from app_main()
'''


def physical_log():
    text = TARGET
    changes = (
        ('profile=qemu', 'profile=physical'), ('synthetic=1', 'synthetic=0'),
        ('plans=3 analyses=3', 'plans=1 analyses=1'), ('storage_psram=0', 'storage_psram=1'),
        ('capacity=20480', 'capacity=4718592'), ('bytes=25866', 'bytes=5898214'),
        (SYNTHETIC_SHA, FM_SHA), ('initial_program=0', 'initial_program=16'),
        ('frames=131072 blocks=2048', 'frames=2097152 blocks=32768'),
        ('events=19', 'events=68'), ('voices_peak=4 loop_voices_peak=2', 'voices_peak=24 loop_voices_peak=24'),
        ('queued=19 note_on=4 note_off=3 cc=10', 'queued=68 note_on=28 note_off=27 cc=11'),
        ('physical=0', 'physical=1'), ('writes=0', 'writes=32768'), ('sent=0', 'sent=32760'),
        ('required=0 blocks=0 voices_min=0 voices_max=0 loops_min=0 loops_max=0',
         'required=1 blocks=4096 voices_min=24 voices_max=24 loops_min=24 loops_max=24'),
        ('gate=NOT_RUN', 'gate=PASS'),
        ('cohort=NOT_RUN', 'cohort=PASS'),
    )
    for old, new in changes:
        text = text.replace(old, new)
    armed = ('REALTIME_I2S_ARMED sample_rate=44100 frames=64 dma_desc=8 dma_frames=64 '
             'dma_window_us=11610 master_q15=32767 dac_db=-12 mono_avg_lr=1 '
             'bclk=1411200 mclk=11289600 dma_bytes=1024\n')
    return text.replace('REALTIME_RESULT', armed + 'REALTIME_RESULT')


def marker(tag, **values):
    return tag + ' ' + ' '.join(f'{key}={value}' for key, value in values.items()) + '\n'


def output_native_log():
    """Parser fixture only: these strings are never runtime evidence."""
    text = NATIVE
    for name in ('exact_bypass_and_master', 'delay_identity_and_drain', 'int32_extremes_invalid_calls',
                 'stereo_linked_smooth_attack_release', 'unchanged_real_fm_matrix_deterministic',
                 'multichannel_musical_replay_deterministic', 'two_channel_concurrent_prepare_stop'):
        text += 'OUTPUT_HOST_CASE ' + name + ' PASS physical_timing_approved=0 listening_approved=0\n'
    for loop in ('off', 'on'):
        raw, held, peak = (9950, 9915, 44381) if loop == 'off' else (4479, 4445, 41708)
        for mode in ('bypass', 'peak'):
            protected = mode == 'peak'
            text += marker('OUTPUT_BANK_RESULT', loop=loop, output=mode, voices=24,
                loops=24 if loop == 'on' else 0, events=68, cohort_raw_overload=held,
                raw_overload_samples=raw, raw_peak_abs=peak,
                post_hard_clipped=0 if protected else raw, post_peak_abs=32766 if protected else peak,
                input_frames=301376, output_frames=301376 + 64 * protected, latency_frames=64 * protected,
                gain_min_q30=800000000 if protected else (1 << 30), limited_frames=100000 if protected else 0,
                checksum=123, deterministic=1, physical_timing_approved=0, listening_approved=0)
            text += marker('OUTPUT_REPLAY_RESULT', loop=loop, output=mode, configured_channels=2,
                channel_mask=3, loop_channel_mask=3 if loop == 'on' else 0, events=117, program_switches=2,
                voices=8, loops=8 if loop == 'on' else 0, channel0_voices=6, channel1_voices=2,
                channel0_loops=6 if loop == 'on' else 0, channel1_loops=2 if loop == 'on' else 0,
                definition_hash=4354443756388215394, trace_hash=11334338450196719549,
                raw_overload_samples=0, raw_peak_abs=15111 if loop == 'off' else 16410,
                post_hard_clipped=0, post_peak_abs=15111 if loop == 'off' else 16410,
                input_frames=245440, output_frames=245440 + 64 * protected, latency_frames=64 * protected,
                gain_min_q30=1 << 30, limited_frames=0, checksum=123,
                deterministic=1, physical_timing_approved=0, listening_approved=0)
    return text + marker('OUTPUT_CONCURRENT_RESULT', configured_channels=2,
        audio_blocks_during_prepare=152, stop_unacked=1, released=1, stale_refs=0, physical_timing_approved=0) + \
        marker('OUTPUT_HOST_PASS', checks=200, cases=7, physical_timing_approved=0, listening_approved=0)


def target_v2_log(profile='qemu', mode='peak', analysis=1):
    """Synthetic parser fixture, including the deliberately failing bypass."""
    physical, protected = profile == 'physical', mode == 'peak'
    text = physical_log() if physical else TARGET
    frames, blocks = (2097152, 32768) if physical else (393216, 6144)
    initial = 16 if physical else 0
    raw, raw_peak, cohort_raw = (4479, 41708, 4445) if physical else (0, 16000, 0)
    post = 0 if protected else raw
    first = 400000 if physical else 64000
    second, cleanup = first + 90240, first + 180480
    changes = [
        ('REALTIME_BEGIN ', 'REALTIME_BEGIN schema=2 output_mode=' + mode + ' channels=2 '),
        ('plans=1 analyses=1', 'plans=1 analyses=1 channel1_plans=1 channel1_analyses=1'),
        ('plans=3 analyses=3', 'plans=3 analyses=3 channel1_plans=3 channel1_analyses=3'),
        ('frames=131072 blocks=2048', f'frames={frames} blocks={blocks}'),
        ('events=68', 'events=186'), ('events=19', 'events=136'),
        ('queued=68 note_on=28 note_off=27 cc=11', 'queued=186 note_on=56 note_off=54 cc=58'),
        ('queued=19 note_on=4 note_off=3 cc=10', 'queued=136 note_on=32 note_off=30 cc=56'),
        ('sustain=4 program_change=2', 'sustain=14 program_change=4 pitch_bend=8 channel_pressure=6'),
        ('prepared=2 acknowledgements=2', 'prepared=4 acknowledgements=4'),
        ('digital_headroom=PASS', f'replay=PASS raw_headroom={"FAIL" if raw else "PASS"} digital_headroom={"FAIL" if post else "PASS"}'),
        ('clipped=0 voices_peak=', f'clipped={post} voices_peak='),
        ('loops_max=0 clipped=0', 'loops_max=0 clipped=0 raw_overload_samples=0'),
        ('loops_max=24 clipped=0', f'loops_max=24 clipped={cohort_raw if not protected else 0} raw_overload_samples={cohort_raw}'),
    ]
    if not physical:
        changes.append((SYNTHETIC_SHA, REPLAY_SYNTHETIC_SHA))
    if physical and protected:
        changes.append(('writes=32768', 'writes=32769'))
    for old, new in changes:
        text = text.replace(old, new)
    text = text.replace('REALTIME_MEMORY phase=before',
        'REALTIME_OUTPUT_PROBE cases=1 input_frames=64 bypass_raw_overload=80 bypass_clipped=80 '
        'peak_raw_overload=80 peak_clipped=0 raw_peak_abs=2147483648 peak_peak_abs=32766 '
        'peak_gain_min_q30=16383 peak_limited_frames=64 peak_latency_frames=64 peak_output_frames=128 '
        'first_zero=1 pass=1 physical_timing_approved=0\nREALTIME_MEMORY phase=before')
    output = marker('REALTIME_OUTPUT', mode=mode, latency_frames=64 * protected, raw_peak_abs=raw_peak,
        raw_overload_samples=raw, post_peak_abs=32766 if protected and physical else raw_peak,
        post_hard_clipped=post, limited_frames=100000 if protected and physical else 0,
        gain_min_q30=800000000 if protected and physical else (1 << 30), input_frames=frames,
        output_frames=frames + 64 * protected, drain_frames=64 * protected, drain_wall_us=100 if protected else 0)
    score = marker('REALTIME_REPLAY', name='two-channel-phrase-v1', events=117,
        definition_hash=replay_hash(initial, 1), event_checksum=replay_hash(initial, 1, first, second, cleanup),
        first_frame=first, second_frame=second, cleanup_frame=cleanup, end_frame=cleanup + 65536,
        program_switches=2, channel_mask=3, loop_channel_mask=3, concurrent_blocks=100,
        prepare_with_other_loop=10, voices_peak=8, loop_voices_peak=8, complete=1)
    text = text.replace('REALTIME_CONTROL', output + score + 'REALTIME_CONTROL')
    if post:
        text = text.replace('REALTIME_PASS runtime_approved=0 physical_timing_approved=0',
                            'REALTIME_FAIL reason=digital_headroom runtime_approved=0 physical_timing_approved=0')
    if not analysis:
        for old, new in (('analysis=1', 'analysis=0'), ('plans=3', 'plans=0'), ('analyses=3', 'analyses=0'),
                         ('plans=1', 'plans=0'), ('analyses=1', 'analyses=0'), ('loops_min=24', 'loops_min=0'),
                         ('loops_max=24', 'loops_max=0'), ('loop_channel_mask=3', 'loop_channel_mask=0'),
                         ('loop_voices_peak=24', 'loop_voices_peak=0'), ('loop_voices_peak=8', 'loop_voices_peak=0'),
                         ('loop_voices_peak=2', 'loop_voices_peak=0'), ('analysis_yields=30', 'analysis_yields=0'),
                         ('prepare_with_other_loop=10', 'prepare_with_other_loop=0')):
            text = text.replace(old, new)
    return text


class Evidence(unittest.TestCase):
    def test_qemu_semantics_do_not_approve_physical_timing(self):
        report = validate(NATIVE, TARGET, 'qemu', 1)
        self.assertEqual(report['source_releases'], 1)
        self.assertFalse(report['physical_execution'])
        self.assertFalse(report['physical_timing_approved'])
        diagnostic = TARGET.replace('max_backlog_us=0 final_backlog_us=0 min_dma_margin_us=11610 exceeded=0',
                                    'max_backlog_us=50000 final_backlog_us=20000 min_dma_margin_us=0 exceeded=1')
        self.assertEqual(validate(NATIVE, diagnostic, 'qemu', 1)['health']['max_backlog_us'], 50000)

    def test_physical_log_requires_actual_cohort_and_dma(self):
        report = validate(NATIVE, physical_log(), 'physical', 1)
        self.assertTrue(report['physical_execution'])
        self.assertFalse(report['release_approved'])
        self.assertEqual(report['cohort']['loops_min'], '24')

    def test_off_is_a_runtime_control(self):
        off = TARGET.replace('analysis=1', 'analysis=0').replace('plans=3 analyses=3', 'plans=0 analyses=0')
        off = off.replace('loop_voices_peak=2', 'loop_voices_peak=0').replace('analysis_yields=30', 'analysis_yields=0')
        off = off.replace('blocks_during_prepare=20', 'blocks_during_prepare=0')
        self.assertEqual(validate(NATIVE, off, 'qemu', 0)['analysis'], 0)

    def test_missing_and_duplicate_records(self):
        for line in TARGET.splitlines():
            with self.subTest(line=line, mode='missing'), self.assertRaises(ValueError):
                validate(NATIVE, TARGET.replace(line + '\n', ''), 'qemu', 1)
            with self.subTest(line=line, mode='duplicate'), self.assertRaises(ValueError):
                validate(NATIVE, TARGET + line + '\n', 'qemu', 1)

    def test_failures_and_false_coverage(self):
        for old, new in (
            ('loop_voices_peak=2', 'loop_voices_peak=0'), ('blocks_during_prepare=20', 'blocks_during_prepare=0'),
            ('note_off=3', 'note_off=0'), ('sustain=4', 'sustain=0'), ('prepared=2', 'prepared=0'),
            ('ack_failures=0', 'ack_failures=1'), ('event_queue_drops=0', 'event_queue_drops=1'),
            ('audio_stack=4000', 'audio_stack=500'), ('mapped=0', 'mapped=1'),
            ('control_stopped=1', 'control_stopped=0'), ('releases=1', 'releases=2'),
            ('REALTIME_ACTORS control_core=0 audio_core=1', 'REALTIME_ACTORS control_core=1 audio_core=0'),
            ('live_refs=0', 'live_refs=1'), ('acquirers=0', 'acquirers=1'),
            ('stale_refs=0', 'stale_refs=1'), ('clipped=0', 'clipped=1'),
            ('runtime_approved=0', 'runtime_approved=1'), ('initial_program=0', 'initial_program=16'),
        ):
            with self.subTest(field=old), self.assertRaises(ValueError):
                validate(NATIVE, TARGET.replace(old, new), 'qemu', 1)
        for marker in ('REALTIME_FAIL', 'Guru Meditation', 'task_wdt:', 'assert failed', 'abort()'):
            with self.subTest(marker=marker), self.assertRaises(ValueError):
                validate(NATIVE, TARGET + marker, 'qemu', 1)

    def test_physical_failure_is_not_hidden_by_old_baseline(self):
        for old, new in (('voices_min=24', 'voices_min=23'), ('loops_min=24', 'loops_min=0'),
                         ('dma_desc=8', 'dma_desc=16'), ('gate=PASS', 'gate=FAIL'),
                         ('dma_queue_overflows=0', 'dma_queue_overflows=1'),
                         ('writes=32768', 'writes=100'), ('final_backlog_us=0', 'final_backlog_us=200')):
            with self.subTest(field=old), self.assertRaises(ValueError):
                validate(NATIVE, physical_log().replace(old, new), 'physical', 1)

    def test_native_can_add_cases_but_cannot_omit_required_ownership(self):
        added = NATIVE.replace('REALTIME_HOST_RESULT suites=10',
                               'REALTIME_HOST_CASE additional_coverage PASS\nREALTIME_HOST_RESULT suites=11')
        self.assertEqual(validate_host(added)['suites'], 11)
        for line in NATIVE.splitlines():
            with self.subTest(line=line), self.assertRaises(ValueError):
                validate_host(NATIVE.replace(line + '\n', ''))


class OutputReplayEvidence(unittest.TestCase):
    def test_full_mode_matrix_has_distinct_output_and_physical_scopes(self):
        native = output_native_log()
        for profile in ('qemu', 'physical'):
            for mode in ('bypass', 'peak'):
                for analysis in (0, 1):
                    failure = profile == 'physical' and mode == 'bypass'
                    with self.subTest(profile=profile, mode=mode, analysis=analysis):
                        report = validate(native, target_v2_log(profile, mode, analysis),
                                          profile, analysis, mode, failure)
                        self.assertEqual(report['schema'], 2)
                        self.assertEqual(report['qualified'], not failure)
                        self.assertEqual(report['observed_hard_gate_pass'], not failure)
                        self.assertFalse(report['runtime_approved'])
                        self.assertFalse(report['physical_timing_approved'])
                        self.assertFalse(report['listening_approved'])
                        self.assertEqual(report['output']['output_frames'],
                                         report['frames'] + (64 if mode == 'peak' else 0))

    def test_old_schema_and_default_mode_cannot_be_silently_upgraded(self):
        self.assertEqual(validate(NATIVE, TARGET, 'qemu', 1)['schema'], 1)
        with self.assertRaises(ValueError):
            validate(output_native_log(), target_v2_log(), 'qemu', 1)
        with self.assertRaises(ValueError):
            validate(output_native_log(), TARGET, 'qemu', 1, 'peak')
        with self.assertRaises(ValueError):
            validate(output_native_log(), target_v2_log(), 'qemu', 1, 'bypass')

    def test_raw_failure_can_only_be_collected_with_all_other_gates_passing(self):
        native, text = output_native_log(), target_v2_log('physical', 'bypass')
        with self.assertRaisesRegex(ValueError, 'fatal target marker'):
            validate(native, text, 'physical', 1, 'bypass')
        for old, new in (('integration=PASS', 'integration=FAIL'), ('cohort=PASS', 'cohort=FAIL'),
                         ('render_max_us=0', 'render_max_us=1452'), ('raw_headroom=FAIL', 'raw_headroom=PASS'),
                         ('live_refs=0', 'live_refs=1'), ('raw_overload_samples=4479', 'raw_overload_samples=0'),
                         ('reason=digital_headroom', 'reason=qualification'), ('partial=0', 'partial=1')):
            with self.subTest(field=old), self.assertRaises(ValueError):
                validate(native, text.replace(old, new), 'physical', 1, 'bypass', True)
        with self.assertRaises(ValueError):
            validate(native, target_v2_log('qemu', 'bypass'), 'qemu', 1, 'bypass', True)
        with self.assertRaises(ValueError):
            validate(native, target_v2_log('physical', 'peak'), 'physical', 1, 'peak', True)

    def test_output_counters_latency_and_exact_score_trace_are_required(self):
        text, native = target_v2_log(), output_native_log()
        for old, new in (
            ('peak_raw_overload=80', 'peak_raw_overload=0'), ('peak_clipped=0', 'peak_clipped=1'),
            ('peak_gain_min_q30=16383', 'peak_gain_min_q30=1073741824'), ('first_zero=1', 'first_zero=0'),
            ('channel1_plans=3', 'channel1_plans=0'), ('channel1_analyses=3', 'channel1_analyses=0'),
            ('latency_frames=64', 'latency_frames=0'), ('drain_frames=64', 'drain_frames=0'),
            ('output_frames=393280', 'output_frames=393216'), ('post_hard_clipped=0', 'post_hard_clipped=1'),
            ('peak_abs=16000', 'peak_abs=40000'), ('gain_min_q30=1073741824', 'gain_min_q30=1073741825'),
            ('replay=PASS', 'replay=FAIL'), ('channel_mask=3', 'channel_mask=1'),
            ('loop_channel_mask=3', 'loop_channel_mask=1'), ('concurrent_blocks=100', 'concurrent_blocks=0'),
            ('prepare_with_other_loop=10', 'prepare_with_other_loop=0'), ('pitch_bend=8', 'pitch_bend=0'),
            ('channel_pressure=6', 'channel_pressure=0'), ('prepared=4', 'prepared=2'),
            ('first_frame=64000', 'first_frame=64064'), ('definition_hash=16085105900585868882', 'definition_hash=1'),
            ('complete=1', 'complete=0'),
        ):
            with self.subTest(field=old), self.assertRaises(ValueError):
                validate(native, text.replace(old, new), 'qemu', 1, 'peak')
        for tag in ('REALTIME_OUTPUT', 'REALTIME_REPLAY', 'REALTIME_OUTPUT_PROBE'):
            line = next(row for row in text.splitlines() if row.startswith(tag + ' '))
            for changed in (text.replace(line + '\n', ''), text + line + '\n'):
                with self.subTest(tag=tag), self.assertRaises(ValueError):
                    validate(native, changed, 'qemu', 1, 'peak')

    def test_host_original_negative_candidate_and_musical_rows_are_all_required(self):
        native = output_native_log()
        self.assertEqual(validate_output_host(native)['cases'], 7)
        for line in native.splitlines():
            if line.startswith(('OUTPUT_HOST_CASE', 'OUTPUT_BANK_RESULT', 'OUTPUT_REPLAY_RESULT', 'OUTPUT_CONCURRENT_RESULT')):
                with self.subTest(missing=line.split()[0]), self.assertRaises(ValueError):
                    validate_output_host(native.replace(line + '\n', ''))
        for old, new in (('raw_overload_samples=9950', 'raw_overload_samples=0'),
                         ('cohort_raw_overload=4445', 'cohort_raw_overload=0'),
                         ('post_hard_clipped=9950', 'post_hard_clipped=0'),
                         ('definition_hash=4354443756388215394', 'definition_hash=1'),
                         ('trace_hash=11334338450196719549', 'trace_hash=1'),
                         ('channel1_loops=2', 'channel1_loops=0'),
                         ('audio_blocks_during_prepare=152', 'audio_blocks_during_prepare=0'),
                         ('physical_timing_approved=0', 'physical_timing_approved=1'),
                         ('listening_approved=0', 'listening_approved=1')):
            with self.subTest(field=old), self.assertRaises(ValueError):
                validate_output_host(native.replace(old, new))


if __name__ == '__main__':
    unittest.main()
