import json
from pathlib import Path
import struct
import tempfile
import unittest

from tools.v1_loop_target_evidence import audit_build, compare_logs, REQUIRED, MACRO


class TargetEvidenceTests(unittest.TestCase):
    def fixture(self, root):
        entries = [dict(directory=str(root), file=name,
                        command=f'xtensa-esp-elf-g++ -D{MACRO}=1 -c {name}')
                   for name in sorted(REQUIRED | {'main/app_main.cpp'})]
        database = root / 'compile_commands.json'
        database.write_text(json.dumps(entries))
        elf = root / 'app.elf'
        elf.write_bytes(b'\x7fELF\x01\x01' + b'\0' * 10 + struct.pack('<HH', 2, 94))
        return entries, database, elf

    def test_enabled_target_build(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            _, database, elf = self.fixture(root)
            result = audit_build(root, database, elf, 'main')
            self.assertEqual(result['owned_cpp_count'], 6)
            self.assertFalse(result['runtime_approved'])

    def test_missing_conflicting_and_undefined_macro_rejected(self):
        for suffix in ('', f'-D{MACRO}=0', f'-D{MACRO}=1 -U{MACRO}',
                       f'-D{MACRO}=0 -D{MACRO}=1', '@flags.rsp'):
            with self.subTest(suffix=suffix), tempfile.TemporaryDirectory() as td:
                root = Path(td)
                entries, database, elf = self.fixture(root)
                entries[0]['command'] = 'xtensa-esp-elf-g++ ' + suffix
                database.write_text(json.dumps(entries))
                with self.assertRaises(ValueError):
                    audit_build(root, database, elf, 'main')

    def test_argument_array_and_split_definition(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            entries, database, elf = self.fixture(root)
            for row in entries:
                row['arguments'] = ['xtensa-esp-elf-g++', '-D', MACRO + '=1']
                del row['command']
            database.write_text(json.dumps(entries))
            self.assertEqual(audit_build(root, database, elf, 'main')['value'], 1)

    def test_missing_unit_or_host_compiler_or_host_elf_rejected(self):
        for mutation in ('unit', 'compiler', 'elf'):
            with self.subTest(mutation=mutation), tempfile.TemporaryDirectory() as td:
                root = Path(td)
                entries, database, elf = self.fixture(root)
                if mutation == 'unit':
                    entries.pop(0)
                elif mutation == 'compiler':
                    entries[0]['command'] = entries[0]['command'].replace('xtensa-esp-elf-g++', 'g++')
                else:
                    elf.write_bytes(b'\x7fELF\x01\x01' + b'\0' * 10 + struct.pack('<HH', 2, 3))
                database.write_text(json.dumps(entries))
                with self.assertRaises(ValueError):
                    audit_build(root, database, elf, 'main')

    def logs(self):
        result = 'V1_TARGET_RESULT cases=72 frames=12345 hash=123456\n'
        target = ('V1_TARGET_BEGIN esp32s3 macro=1 synthetic_only=1\n' + result +
                  'V1_TARGET_LAYOUT pointer=4 voice=800 params=540 pool=26000\nV1_TARGET_PASS\n')
        return result, target

    def test_exact_target_execution(self):
        native, target = self.logs()
        data = compare_logs(native, target)
        self.assertTrue(data['native_target_equal'])
        self.assertFalse(data['hardware_timing_approved'])

    def test_incomplete_duplicate_fatal_and_mismatch_rejected(self):
        native, target = self.logs()
        variants = [target.replace('V1_TARGET_PASS', ''), target + target,
                    target + 'Guru Meditation', target + 'task_wdt:',
                    target.replace('hash=123456', 'hash=123457'),
                    target.replace('pointer=4', 'pointer=8'),
                    target.replace('V1_TARGET_PASS', 'assert failed')]
        for text in variants:
            with self.subTest(text=text), self.assertRaises(ValueError):
                compare_logs(native, text)


if __name__ == '__main__':
    unittest.main()
