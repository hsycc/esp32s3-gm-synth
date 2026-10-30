"""Exercise real shell/manifest generation and repeat-build artifact handling.

IDF, audit and packaging stubs test orchestration, not compilation, target
execution or hardware. The test-only shell override also permits GNU Bash 3.2.
"""
import hashlib
from itertools import product
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BANK = 'assets/sf2/subsets/FM_GM_Open_subset_6MiB.sf2'
HASHES = {
    'qemu': '98c6ed2230abd112e073996c1a1747e47e4eef1e72c6894c2cb0ca4e6750431c',
    'physical': '9650eb1c2202e2d92aa3be155e4e895cf6531a241c8ea413fc09e7f9159d4a62',
}
FILES = (
    'scripts/build_v1_loop_realtime.sh', 'tools/v1_loop_realtime_manifest.py',
    'tests/__init__.py', 'tests/app/make_loop_replay_fixture.py',
    'tests/app/make_loop_analysis_fixture.py', 'tests/app/make_loop_app_fixture.py',
    'tools/v1_loop_policy.py', 'tools/v1_loop_crossfade_planner.py',
    'tools/m7_loop_seam_audio.py', 'tools/v1_loop_matched_peak.py', BANK,
)
BUILD_STOP = 73
IDF_STUB = '''import json, os, sys
from pathlib import Path
if sys.argv[1:] == ['--version']:
    print(os.environ['V1_REALTIME_TEST_IDF_VERSION'])
    sys.exit(0)
if not sys.argv[1:] or sys.argv[-1] != 'build':
    sys.exit(74)
Path(os.environ['V1_REALTIME_TEST_ARGV']).write_text(json.dumps(sys.argv[1:]))
target = Path(sys.argv[sys.argv.index('-B') + 1])
sdkconfig = Path(next(arg.split('=', 1)[1] for arg in sys.argv if arg.startswith('SDKCONFIG=')))
if target.exists() or sdkconfig.exists():
    sys.exit('IDF TEST STUB: stale target or sdkconfig reused')
target.mkdir(parents=True)
marker = 'IDF TEST STUB: ' + os.environ['V1_REALTIME_TEST_RUN'] + '\\n'
(target / 'CMakeCache.txt').write_text(marker)
sdkconfig.write_text(marker)
print('IDF TEST STUB: build boundary only; no compilation')
sys.exit(int(os.environ['V1_REALTIME_TEST_BUILD_RC']))
'''
AUDIT_STUB = '''import json, os, sys
from pathlib import Path
output = Path(sys.argv[sys.argv.index('--output') + 1])
output.write_text(json.dumps(dict(test_stub=True, run=os.environ['V1_REALTIME_TEST_RUN'])))
'''
BUNDLE_STUB = '''import json, os, sys
from pathlib import Path
output = Path(sys.argv[sys.argv.index('--output') + 1])
output.mkdir()
(output / 'bundle-manifest.json').write_text(json.dumps(
    dict(test_stub=True, run=os.environ['V1_REALTIME_TEST_RUN'])))
(output / 'images').mkdir()
(output / 'images/test-stub.bin').write_bytes(b'not firmware')
'''


class RealtimeShell(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='v1 realtime shell ')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        for relative in FILES:
            destination = self.root / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / relative, destination)
        # Keep both Python commands on this interpreter, including on macOS.
        binaries = self.root / 'python commands'
        binaries.mkdir()
        for name in ('python', 'python3'):
            (binaries / name).symlink_to(sys.executable)
        idf = self.root / 'stub IDF'
        (idf / 'tools').mkdir(parents=True)
        (idf / 'tools/idf.py').write_text(IDF_STUB)
        for name in ('v1_loop_target_evidence', 'v1_atomic_backend_audit'):
            (self.root / f'tools/{name}.py').write_text(AUDIT_STUB)
        (self.root / 'tools/v1_loop_realtime_bundle.py').write_text(BUNDLE_STUB)
        self.record = self.root / 'build-argv.json'
        self.env = dict(os.environ, IDF_PATH=str(idf), PYTHONPATH='',
                        PATH=str(binaries) + os.pathsep + os.environ.get('PATH', ''),
                        V1_REALTIME_TEST_ARGV=str(self.record),
                        V1_REALTIME_TEST_IDF_VERSION='ESP-IDF v6.1',
                        V1_REALTIME_TEST_BUILD_RC=str(BUILD_STOP),
                        V1_REALTIME_TEST_RUN='first')
        self.shell = os.environ.get('V1_REALTIME_TEST_SHELL', '/bin/bash')

    def output(self, profile, analysis, mode):
        return self.root / f'build/v1-loop-realtime-{profile}-{analysis}-{mode}'

    def archives(self, profile, analysis, mode):
        history = self.root / 'build/v1-loop-realtime-history'
        return sorted(path / 'build' for path in history.glob(f'{profile}-{analysis}-{mode}.*'))

    @staticmethod
    def snapshot(directory):
        return {path.relative_to(directory).as_posix(): path.read_bytes() if path.is_file() else None
                for path in directory.rglob('*')}

    def run_build(self, profile='physical', analysis='off', mode='peak'):
        return subprocess.run([self.shell, str(self.root / FILES[0]), profile, analysis, mode],
                              cwd=self.root, env=self.env, capture_output=True, text=True, timeout=60)

    def assert_inputs(self, profile, analysis, mode):
        out = self.output(profile, analysis, mode)
        expected = ['-B', str(out / 'target'), '-D', 'IDF_TARGET=esp32s3',
                    '-D', f'SDKCONFIG={out}/sdkconfig',
                    '-D', f'SDKCONFIG_DEFAULTS={self.root}/profiles/v1-loop-realtime/sdkconfig.common;'
                          f'{self.root}/profiles/v1-loop-realtime/sdkconfig.{profile}',
                    '-D', 'GM_SYNTH_BUILD_LOOP_REALTIME_QUAL=ON',
                    '-D', f'GM_SYNTH_REALTIME_GENERATED={out}/generated', 'build']
        self.assertEqual(json.loads(self.record.read_text()), expected)
        manifest = json.loads((out / 'generated/manifest.json').read_text())
        source = out / 'fixture/fixture.sf2' if profile == 'qemu' else self.root / BANK
        data = source.read_bytes()
        self.assertEqual(hashlib.sha256(data).hexdigest(), HASHES[profile])
        values = dict(schema=2, profile=profile, analysis=analysis == 'on',
                      output_mode=mode, output_latency_frames=64 if mode == 'peak' else 0,
                      sha256=HASHES[profile], bytes=len(data))
        self.assertEqual({key: manifest[key] for key in values}, values)
        self.assertEqual((out / 'generated/bank.sf2').read_bytes(), data)
        self.assertTrue((out / 'generated/loop_realtime_manifest.h').is_file())
        self.assertEqual((out / 'idf-version.txt').read_text(), 'ESP-IDF v6.1\n')
        self.assertTrue((out / 'target/CMakeCache.txt').is_file())

    def test_all_modes_preserve_arguments_and_retry_failed_build(self):
        for profile, analysis, mode in product(('qemu', 'physical'), ('off', 'on'), ('bypass', 'peak')):
            with self.subTest(profile=profile, analysis=analysis, output=mode):
                out = self.output(profile, analysis, mode)
                # CI creates only this directory/log before invoking the script.
                out.mkdir(parents=True)
                (out / 'build.log').write_text('existing CI log\n')
                self.env['V1_REALTIME_TEST_RUN'] = 'first'
                run = self.run_build(profile, analysis, mode)
                self.assertEqual(run.returncode, BUILD_STOP, run.stdout + run.stderr)
                self.assertIn('IDF TEST STUB: build boundary only', run.stdout)
                self.assert_inputs(profile, analysis, mode)
                self.assertEqual(self.archives(profile, analysis, mode), [])
                self.assertEqual((out / 'build.log').read_text(), 'existing CI log\n')
                before = self.snapshot(out)
                self.env['V1_REALTIME_TEST_RUN'] = 'retry'
                retry = self.run_build(profile, analysis, mode)
                self.assertEqual(retry.returncode, BUILD_STOP, retry.stdout + retry.stderr)
                self.assert_inputs(profile, analysis, mode)
                archives = self.archives(profile, analysis, mode)
                self.assertEqual(len(archives), 1)
                self.assertEqual(self.snapshot(archives[0]), before)
                self.assertIn(str(archives[0]), retry.stdout)
                self.assertEqual((out / 'target/CMakeCache.txt').read_text(), 'IDF TEST STUB: retry\n')
                self.assertFalse((out / 'build.log').exists())
                self.assertFalse((out / 'build-audit.json').exists())
                self.assertFalse((out / 'flash-bundle').exists())

    def test_successful_bundle_and_runtime_evidence_are_preserved_on_rebuild(self):
        self.env['V1_REALTIME_TEST_BUILD_RC'] = '0'
        out = self.output('physical', 'off', 'peak')
        first = self.run_build()
        self.assertEqual(first.returncode, 0, first.stdout + first.stderr)
        self.assert_inputs('physical', 'off', 'peak')
        self.assertTrue((out / 'flash-bundle/bundle-manifest.json').is_file())
        # Simulate user-edited configuration and evidence from an earlier run.
        (out / 'sdkconfig').write_text('CONFIG_TEST_OLD_SETTING=y\n')
        (out / 'execution.json').write_text('{"old_execution": true}\n')
        (out / 'qemu.log').write_text('previous runtime output\n')
        (out / 'build.log').write_text('previous successful build\n')
        (out / 'operator notes.txt').write_text('keep these notes\n')
        before = self.snapshot(out)
        self.env['V1_REALTIME_TEST_RUN'] = 'second'
        second = self.run_build()
        self.assertEqual(second.returncode, 0, second.stdout + second.stderr)
        self.assert_inputs('physical', 'off', 'peak')
        archives = self.archives('physical', 'off', 'peak')
        self.assertEqual(len(archives), 1)
        self.assertEqual(self.snapshot(archives[0]), before)
        self.assertEqual((out / 'sdkconfig').read_text(), 'IDF TEST STUB: second\n')
        self.assertEqual(json.loads((out / 'flash-bundle/bundle-manifest.json').read_text()),
                         dict(test_stub=True, run='second'))
        for name in ('execution.json', 'qemu.log', 'build.log', 'operator notes.txt'):
            self.assertFalse((out / name).exists(), name)

        # A later failure must not leave the previous successful bundle looking current.
        (out / 'execution.json').write_text('{"second_execution": true}\n')
        second_snapshot = self.snapshot(out)
        other = self.output('physical', 'on', 'peak')
        other.mkdir(parents=True)
        (other / 'untouched.txt').write_text('another matrix combination\n')
        other_snapshot = self.snapshot(other)
        self.env['V1_REALTIME_TEST_RUN'] = 'failed after success'
        self.env['V1_REALTIME_TEST_BUILD_RC'] = str(BUILD_STOP)
        failed = self.run_build()
        self.assertEqual(failed.returncode, BUILD_STOP, failed.stdout + failed.stderr)
        self.assert_inputs('physical', 'off', 'peak')
        self.assertCountEqual([self.snapshot(path) for path in self.archives('physical', 'off', 'peak')],
                              [before, second_snapshot])
        for name in ('flash-bundle', 'execution.json', 'build-audit.json', 'atomic-backend-audit.json'):
            self.assertFalse((out / name).exists(), name)
        self.assertEqual(self.snapshot(other), other_snapshot)

    def test_repeated_failures_keep_separate_complete_histories(self):
        out = self.output('physical', 'off', 'bypass')
        previous = []
        for index, label in enumerate(('first', 'second', 'third')):
            self.env['V1_REALTIME_TEST_RUN'] = label
            run = self.run_build(mode='bypass')
            self.assertEqual(run.returncode, BUILD_STOP, run.stdout + run.stderr)
            self.assert_inputs('physical', 'off', 'bypass')
            archives = self.archives('physical', 'off', 'bypass')
            self.assertEqual(len(archives), index)
            archived = [self.snapshot(path) for path in archives]
            self.assertCountEqual(archived, previous)
            previous.append(self.snapshot(out))
            self.assertFalse((out / 'flash-bundle').exists())

    def test_wrong_idf_version_does_not_modify_or_archive_existing_build(self):
        out = self.output('physical', 'off', 'peak')
        (out / 'target').mkdir(parents=True)
        (out / 'flash-bundle').mkdir()
        for relative in ('target/CMakeCache.txt', 'sdkconfig', 'idf-version.txt',
                         'flash-bundle/bundle-manifest.json', 'execution.json', 'build.log'):
            (out / relative).write_text('preserve ' + relative + '\n')
        before = self.snapshot(out)
        for version in ('ESP-IDF v6.2', 'ESP-IDF v6.1-dirty'):
            with self.subTest(version=version):
                self.env['V1_REALTIME_TEST_IDF_VERSION'] = version
                run = self.run_build()
                self.assertNotEqual(run.returncode, 0, run.stdout + run.stderr)
                self.assertNotEqual(run.returncode, BUILD_STOP, run.stdout + run.stderr)
                self.assertEqual(self.snapshot(out), before)
                self.assertFalse(self.record.exists())
                self.assertEqual(self.archives('physical', 'off', 'peak'), [])


if __name__ == '__main__':
    unittest.main()
