"""Address policy tests use Host stubs, not a hardware/PSRAM execution claim."""
import os
from pathlib import Path
import resource
import shutil
import signal
import subprocess
import tempfile
import unittest

from scripts.v1_atomic_contract_source import PATCHES, restore_atomic_contract

ROOT = Path(__file__).resolve().parents[1]
SOURCE = r'''
#include "util/RealtimeAtomicContract.h"
#include <cstring>
#include <limits>
#if defined(ESP_PLATFORM)
std::uintptr_t test_dram_begin=0, test_dram_end=0, test_ext_begin=0, test_ext_end=0;
#endif
int main(int argc, char** argv) {
    using namespace gm_synth::util;
    std::atomic<std::uint32_t> word{7};
    const auto p=reinterpret_cast<std::uintptr_t>(&word);
#if defined(ESP_PLATFORM)
    test_dram_begin=p; test_dram_end=p+4;
    test_ext_begin=1; test_ext_end=2;
    if (argc>1 && !std::strcmp(argv[1],"reject")) {
        test_ext_begin=p;test_ext_end=p+4;
        requireRealtimeAtomic32(word);return 99;
    }
#endif
    (void)argc;(void)argv;
    if (!realtimeAtomic32StorageAllowed(&word)) return 1;
    requireRealtimeAtomic32(word);
    word.store(11,std::memory_order_release);
    if (word.load(std::memory_order_acquire)!=11) return 2;
    if (realtimeAtomic32StorageAllowed(nullptr) ||
        realtimeAtomic32StorageAllowed(reinterpret_cast<const void*>(p+1)) ||
        realtimeAtomic32StorageAllowed(reinterpret_cast<const void*>(std::numeric_limits<std::uintptr_t>::max()))) return 3;
#if defined(ESP_PLATFORM)
    // The whole word must fit in ordinary DRAM.
    test_dram_end=p+3;
    if (realtimeAtomic32StorageAllowed(&word)) return 4;
    test_dram_end=p+4;
    test_dram_begin=p+1;
    if (realtimeAtomic32StorageAllowed(&word)) return 5;
    test_dram_begin=p;
    // A heap/internal alias inside IDF's EXTRAM range is still forbidden.
    test_ext_begin=p;test_ext_end=p+4;
    if (realtimeAtomic32StorageAllowed(&word)) return 6;
    test_ext_begin=p+3;
    if (realtimeAtomic32StorageAllowed(&word)) return 7;
    test_ext_begin=p+4;test_ext_end=p+8;
    if (!realtimeAtomic32StorageAllowed(&word)) return 8;
    test_ext_begin=p-4;test_ext_end=p;
    if (!realtimeAtomic32StorageAllowed(&word)) return 9;
#endif
    return 0;
}
'''

class AddressContract(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.dir = Path(cls.temp.name)
        cls.compiler = os.environ.get('CXX', 'g++')
        if not shutil.which(cls.compiler):
            raise RuntimeError('required compiler unavailable: ' + cls.compiler)
        (cls.dir/'test.cpp').write_text(SOURCE)
        (cls.dir/'sdkconfig.h').write_text('#define CONFIG_STDATOMIC_S32C1I_SPIRAM_WORKAROUND 1\n#define CONFIG_IDF_TARGET_ESP32S3 1\n')
        (cls.dir/'esp_idf_version.h').write_text('#define ESP_IDF_VERSION_MAJOR 6\n#define ESP_IDF_VERSION_MINOR 1\n')
        (cls.dir/'esp_memory_utils.h').write_text('''#pragma once
#include <cstdint>
extern std::uintptr_t test_dram_begin, test_dram_end, test_ext_begin, test_ext_end;
inline bool esp_ptr_in_dram(const void* p) {auto v=reinterpret_cast<std::uintptr_t>(p);return v>=test_dram_begin && v<test_dram_end;}
#define SOC_EXTRAM_DATA_LOW test_ext_begin
#define SOC_EXTRAM_DATA_HIGH test_ext_end
''')
        for mode in ('host','stub'):
            command=[cls.compiler,'-std=c++17','-O2','-Wall','-Wextra','-Werror','-pedantic','-DNDEBUG',
                     '-I'+str(ROOT/'firmware'),'-I'+str(cls.dir),str(cls.dir/'test.cpp'),'-o',str(cls.dir/mode)]
            if mode=='stub':command+=['-DESP_PLATFORM=1','-D__XTENSA__=1']
            # Clang is intentionally unsupported for the GCC-workaround branch.
            if mode=='stub' and 'clang' in cls.compiler:
                command[0]='g++'
            subprocess.run(command,check=True,capture_output=True,text=True)
    @classmethod
    def tearDownClass(cls):cls.temp.cleanup()
    def test_native_type_and_load_store(self):
        subprocess.run([self.dir/'host'],check=True)
    def test_esp_address_range_alignment_aliases(self):
        subprocess.run([self.dir/'stub'],check=True)
    def test_invalid_placement_aborts_even_under_ndebug(self):
        def no_core():resource.setrlimit(resource.RLIMIT_CORE,(0,0))
        result=subprocess.run([self.dir/'stub','reject'],preexec_fn=no_core,capture_output=True)
        self.assertEqual(result.returncode,-signal.SIGABRT)
    def test_sdk_series_change_fails_closed(self):
        version=self.dir/'esp_idf_version.h'
        saved=version.read_text()
        try:
            version.write_text(saved.replace('MINOR 1','MINOR 2'))
            result=subprocess.run(['g++','-std=c++17','-DESP_PLATFORM=1','-D__XTENSA__=1',
                '-I'+str(ROOT/'firmware'),'-I'+str(self.dir),'-fsyntax-only',str(self.dir/'test.cpp')],capture_output=True,text=True)
            self.assertNotEqual(result.returncode,0)
            self.assertIn('Re-audit',result.stderr)
        finally:version.write_text(saved)
    def test_unqualified_workaround_target_fails_closed(self):
        config=self.dir/'sdkconfig.h';saved=config.read_text()
        try:
            config.write_text(saved.replace('ESP32S3 1','ESP32S3 0'))
            result=subprocess.run(['g++','-std=c++17','-DESP_PLATFORM=1','-D__XTENSA__=1',
                '-I'+str(ROOT/'firmware'),'-I'+str(self.dir),'-fsyntax-only',str(self.dir/'test.cpp')],capture_output=True,text=True)
            self.assertNotEqual(result.returncode,0)
            self.assertIn('audited only',result.stderr)
        finally:config.write_text(saved)

class SourceContract(unittest.TestCase):
    def test_two_owner_constructors_enforce_storage(self):
        for path in PATCHES:
            text=(ROOT/path).read_text()
            self.assertEqual(text.count('util::requireRealtimeAtomic32(phase_);'),1)
            restored=restore_atomic_contract(path,text)
            self.assertIn('std::atomic<std::uint32_t>::is_always_lock_free',restored)
            self.assertNotIn('requireRealtimeAtomic32',restored)
    def test_missing_or_duplicated_enforcement_is_rejected(self):
        for path in PATCHES:
            text=(ROOT/path).read_text();call='util::requireRealtimeAtomic32(phase_);'
            for replacement in ('',call+call):
                with self.subTest(path=path,replacement=replacement),self.assertRaises(ValueError):
                    restore_atomic_contract(path,text.replace(call,replacement))
    def test_protocol_change_is_not_normalized_away(self):
        for path in PATCHES:
            text=(ROOT/path).read_text()
            changed=text.replace('std::memory_order_release','std::memory_order_relaxed',1)
            self.assertNotEqual(restore_atomic_contract(path,text),restore_atomic_contract(path,changed))
    def test_state_type_and_orders_are_preserved(self):
        for path in PATCHES:
            text=(ROOT/path).read_text();restored=restore_atomic_contract(path,text)
            self.assertIn('std::atomic<std::uint32_t> phase_',text)
            import re
            self.assertEqual(re.findall(r'phase_\.(?:load|store)\([^;]*',text),
                             re.findall(r'phase_\.(?:load|store)\([^;]*',restored))
    def test_no_compiler_flag_workaround_disabled(self):
        for path in ('profiles/v1-loop-startup/sdkconfig.physical','scripts/build_v1_loop_startup.sh','CMakeLists.txt'):
            text=(ROOT/path).read_text()
            self.assertNotIn('-mno-disable-hardware-atomics',text)
            self.assertNotIn('CONFIG_STDATOMIC_S32C1I_SPIRAM_WORKAROUND=n',text)


class BackendAudit(unittest.TestCase):
    def test_simple_hardware_body(self):
        from tools.v1_atomic_backend_audit import checked_body
        self.assertIn('memw',checked_body('00000000 <load>:\n 0: 0020c0 memw\n 3: 0328 l32i.n a2, a3, 0\n 5: f00d retw.n\n','load'))
    def test_rejects_lock_call_and_backwards_branch(self):
        from tools.v1_atomic_backend_audit import checked_body
        for instruction in ('3: 000085 call8 10 <lock>','3: ffffc6 j 0 <load>','3: 000000 ill'):
            with self.subTest(instruction=instruction),self.assertRaises(ValueError):
                checked_body('00000000 <load>:\n '+instruction+'\n 6: f00d retw.n\n','load')
    def test_missing_body_is_not_pass(self):
        from tools.v1_atomic_backend_audit import checked_body
        with self.assertRaises(ValueError):checked_body('no symbol','load')
    def test_sdk_routing_change_rejected(self):
        from tools.v1_atomic_backend_audit import source_contract
        with self.assertRaises(ValueError):source_contract('#define _ATOMIC_IF_NOT_EXT_RAM() if (true)')

if __name__=='__main__':unittest.main()
