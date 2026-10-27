import tempfile
from pathlib import Path
import unittest
from tools.v1_loop_startup_evidence import validate,DIGEST
from tools.v1_loop_startup_manifest import generate
from tests.app.make_loop_analysis_fixture import generate as fixture
NATIVE='STARTUP_HOST suites=16 checks=2270 automatic=2034267099694268986 direct=14037194901542623014 frames=16384\nSTARTUP_HOST_PASS\n'
TARGET=f'''STARTUP_BEGIN profile=qemu analysis=1 root_application=1
STARTUP_READY status=1 session=1 plans=3 analyzed=3 work=100 reads=100 arena_capacity=20480 session_bytes=300000
STARTUP_ASSET bytes=25866 sha256={DIGEST}
STARTUP_RESULT frames=16384 trace=2034267099694268986 initial_loops=1 replacement_fade=0 events=3 rejected=0
STARTUP_TIMING startup_us=1 hash_us=1 service_max_us=1 render_avg_us=1 render_max_us=1 diagnostic_only=1
STARTUP_PCM capacity=20480 high_water=17000 used=17000 allocation_failures=0
STARTUP_STOP releases=1 mapped=0 arena_live=0
STARTUP_PASS runtime_approved=0
I (123) main_task: Returned from app_main()
'''
class Evidence(unittest.TestCase):
    def test_valid(self):self.assertEqual(validate(NATIVE,TARGET,1)['source_releases'],1)
    def test_failures(self):
        for bad in ['STARTUP_FAIL','Guru Meditation','task_wdt:','assert failed']:
            with self.subTest(bad=bad),self.assertRaises(ValueError):validate(NATIVE,TARGET+'\n'+bad,1)
    def test_incomplete(self):
        for line in TARGET.splitlines():
            with self.subTest(line=line),self.assertRaises(ValueError):validate(NATIVE,TARGET.replace(line+'\n',''),1)
    def test_duplicate(self):
        for line in TARGET.splitlines():
            with self.subTest(line=line),self.assertRaises(ValueError):validate(NATIVE,TARGET+line+'\n',1)
    def test_wrong_values(self):
        for a,b in [('mapped=0','mapped=1'),('releases=1','releases=2'),('session=1','session=0'),('events=3','events=0'),
                    ('2034267099694268986','2034267099694268987'),('analysis=1','analysis=0'),('high_water=17000','high_water=25000')]:
            with self.subTest(a=a),self.assertRaises(ValueError):validate(NATIVE,TARGET.replace(a,b),1)
    def test_off(self):
        off=TARGET.replace('analysis=1','analysis=0').replace('plans=3 analyzed=3 work=100 reads=100','plans=0 analyzed=0 work=0 reads=0').replace('initial_loops=1','initial_loops=0').replace('2034267099694268986','14037194901542623014')
        self.assertEqual(validate(NATIVE,off,0)['analysis'],0)
    def test_physical_not_mislabelled(self):
        with self.assertRaises(ValueError):validate(NATIVE,TARGET.replace('profile=qemu','profile=physical'),1)
    def test_order(self):
        with self.assertRaises(ValueError):validate(NATIVE,'STARTUP_PASS runtime_approved=0\n'+TARGET.replace('STARTUP_PASS runtime_approved=0\n',''),1)
class Manifest(unittest.TestCase):
    def test_pinned_fixture(self):
        with tempfile.TemporaryDirectory() as t:
            t=Path(t);fixture(t/'fixture');s=t/'fixture/fixture.sf2'
            self.assertEqual(generate(s,t/'out','qemu'),DIGEST)
            self.assertIn('analysis=false',(t/'out/loop_startup_manifest.h').read_text())
            self.assertEqual((t/'out/bank.sf2').read_bytes(),s.read_bytes())
    def test_wrong_resource(self):
        with tempfile.TemporaryDirectory() as t:
            t=Path(t);fixture(t/'fixture');s=t/'fixture/fixture.sf2'
            with self.assertRaises(ValueError):generate(s,t/'out','physical',True)
            b=bytearray(s.read_bytes());b[-1]^=1;s.write_bytes(b)
            with self.assertRaises(ValueError):generate(s,t/'out','qemu',True)
    def test_bad_container(self):
        with tempfile.TemporaryDirectory() as t:
            p=Path(t)/'bad';p.write_bytes(b'RIFF')
            with self.assertRaises(ValueError):generate(p,Path(t)/'out','qemu')

class EarlyDependencyScan(unittest.TestCase):
    def scan(self,mode):
        import os
        import subprocess
        root=Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as temp:
            temp=Path(temp);generated=temp/'generated';generated.mkdir()
            for name in ('loop_startup_manifest.h','bank.sf2'):(generated/name).touch()
            early=temp/'early.cmake'
            early.write_text('function(idf_component_register)\n'
                ' file(WRITE "'+(temp/'requirements.txt').as_posix()+'" "${ARGV}")\n'
                'endfunction()\ninclude("'+(root/'main/CMakeLists.txt').as_posix()+'")\n')
            prefix=(root/'CMakeLists.txt').read_text().split('set(EXTRA_COMPONENT_DIRS',1)[0]
            # Run real configure then a fresh script-mode child, as IDF does.
            (temp/'CMakeLists.txt').write_text(prefix+'\nproject(scan NONE)\n'
                'execute_process(COMMAND "${CMAKE_COMMAND}" -P "'+early.as_posix()+'" RESULT_VARIABLE result)\n'
                'if(NOT result EQUAL 0)\nmessage(FATAL_ERROR "early child failed")\nendif()\n')
            env=dict(os.environ,GM_SYNTH_STARTUP_EARLY_MODE='1',GM_SYNTH_STARTUP_EARLY_GENERATED='/invalid/inherited/path')
            subprocess.run(['cmake','-S',str(temp),'-B',str(temp/'build'),
                '-DGM_SYNTH_BUILD_LOOP_STARTUP_QUAL='+mode,'-DGM_SYNTH_STARTUP_GENERATED='+str(generated)],
                env=env,check=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
            return (temp/'requirements.txt').read_text(),str(generated)
    def test_enabled_dependencies_reach_early_child(self):
        for mode in ('ON','TRUE','1'):
            with self.subTest(mode=mode):
                args,generated=self.scan(mode)
                self.assertIn('LoopStartupQualification.cpp',args)
                self.assertIn('mbedtls',args);self.assertIn(generated,args)
                self.assertNotIn('app_main.cpp',args)
    def test_disabled_overrides_inherited_enable(self):
        args,_=self.scan('OFF')
        self.assertIn('app_main.cpp',args);self.assertNotIn('LoopStartupQualification.cpp',args)
        self.assertNotIn('mbedtls',args);self.assertNotIn('/invalid/inherited/path',args)
