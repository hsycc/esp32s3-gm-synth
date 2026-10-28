"""Execute the real shell/manifest path, stopping at a deliberately failing IDF stub.

This checks Bash argument handling, not compilation, target execution or hardware.
The test-only shell override permits running the same case with GNU Bash 3.2.
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
    print('ESP-IDF v6.1')
    sys.exit(0)
if not sys.argv[1:] or sys.argv[-1] != 'build':
    sys.exit(74)
Path(os.environ['V1_REALTIME_TEST_ARGV']).write_text(json.dumps(sys.argv[1:]))
print('IDF TEST STUB: build boundary only; no compilation')
sys.exit(73)
'''


class RealtimeShell(unittest.TestCase):
    def test_all_modes_preserve_arguments_and_build_failure(self):
        shell = os.environ.get('V1_REALTIME_TEST_SHELL', '/bin/bash')
        with tempfile.TemporaryDirectory(prefix='v1 realtime shell ') as temporary:
            root = Path(temporary).resolve()
            for relative in FILES:
                destination = root / relative
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(ROOT / relative, destination)
            # Keep both Python commands on this test's interpreter, including on macOS.
            binaries = root / 'python commands'
            binaries.mkdir()
            for name in ('python', 'python3'):
                (binaries / name).symlink_to(sys.executable)
            idf = root / 'stub IDF'
            (idf / 'tools').mkdir(parents=True)
            (idf / 'tools/idf.py').write_text(IDF_STUB)
            env = dict(os.environ, IDF_PATH=str(idf), PYTHONPATH='',
                       PATH=str(binaries) + os.pathsep + os.environ.get('PATH', ''))
            for profile, analysis, mode in product(('qemu', 'physical'), ('off', 'on'), ('bypass', 'peak')):
                with self.subTest(profile=profile, analysis=analysis, output=mode):
                    out = root / f'build/v1-loop-realtime-{profile}-{analysis}-{mode}'
                    record = root / f'{profile}-{analysis}-{mode}-argv.json'
                    env['V1_REALTIME_TEST_ARGV'] = str(record)
                    run = subprocess.run([shell, str(root / FILES[0]), profile, analysis, mode],
                                         cwd=root, env=env, capture_output=True, text=True, timeout=60)
                    self.assertEqual(run.returncode, BUILD_STOP, run.stdout + run.stderr)
                    self.assertIn('IDF TEST STUB: build boundary only', run.stdout)
                    expected = ['-B', str(out / 'target'), '-D', 'IDF_TARGET=esp32s3',
                                '-D', f'SDKCONFIG={out}/sdkconfig',
                                '-D', f'SDKCONFIG_DEFAULTS={root}/profiles/v1-loop-realtime/sdkconfig.common;'
                                      f'{root}/profiles/v1-loop-realtime/sdkconfig.{profile}',
                                '-D', 'GM_SYNTH_BUILD_LOOP_REALTIME_QUAL=ON',
                                '-D', f'GM_SYNTH_REALTIME_GENERATED={out}/generated', 'build']
                    self.assertEqual(json.loads(record.read_text()), expected)
                    manifest = json.loads((out / 'generated/manifest.json').read_text())
                    source = out / 'fixture/fixture.sf2' if profile == 'qemu' else root / BANK
                    data = source.read_bytes()
                    self.assertEqual(hashlib.sha256(data).hexdigest(), HASHES[profile])
                    values = dict(schema=2, profile=profile, analysis=analysis == 'on',
                                  output_mode=mode, output_latency_frames=64 if mode == 'peak' else 0,
                                  sha256=HASHES[profile], bytes=len(data))
                    self.assertEqual({key: manifest[key] for key in values}, values)
                    self.assertEqual((out / 'generated/bank.sf2').read_bytes(), data)
                    self.assertTrue((out / 'generated/loop_realtime_manifest.h').is_file())
                    self.assertEqual((out / 'idf-version.txt').read_text(), 'ESP-IDF v6.1\n')
                    self.assertFalse((out / 'target').exists())
                    self.assertFalse((out / 'build-audit.json').exists())
                    self.assertFalse((out / 'flash-bundle').exists())


if __name__ == '__main__':
    unittest.main()
