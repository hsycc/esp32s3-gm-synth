import os
from pathlib import Path
import random
import subprocess
import tempfile
import unittest
from tools.v1_loop_executor_check import Q, check_geometry, pcm_bytes, reference


class CursorOracleTests(unittest.TestCase):
    def test_direct_sign_and_fraction(self):
        self.assertEqual(reference([100,-101,300,-32768],0,Q//2,4),pcm_bytes([100,-1,-101,99]))

    def test_until_release_uses_raw_tail(self):
        pcm=list(range(200))
        data=reference(pcm,32,Q,500,180)
        self.assertEqual(len(data),400)
        self.assertEqual(data[360:],pcm_bytes(pcm[180:]))

    def test_exact_integer_blend_and_resume(self):
        pcm=[0]*100+[1000]*100
        out=reference(pcm,16,Q,203)
        self.assertEqual(out[368:400],pcm_bytes([round(1000*(16-k)/17) for k in range(16)]))
        self.assertEqual(out[400:],pcm_bytes([0,0,0]))


@unittest.skipUnless(os.environ.get('V1_LOOP_CURSOR_PROBE'),'compiled probe required')
class NativeCursorOracleTests(unittest.TestCase):
    def test_multiple_rates_blocks_and_release(self):
        rng=random.Random(73)
        for n in (200,512,1601):
            pcm=[rng.randint(-32768,32767) for _ in range(n)]
            for fade in (0,16,32,64):
                result=check_geometry(Path(os.environ['V1_LOOP_CURSOR_PROBE']),pcm,fade)
                self.assertEqual(len(result),6)
                self.assertTrue(all(c['equal'] for c in result))

    def test_probe_rejects_bad_arguments(self):
        binary=os.environ['V1_LOOP_CURSOR_PROBE']
        with tempfile.TemporaryDirectory() as td:
            pcm=Path(td)/'data.pcm'; pcm.write_bytes(pcm_bytes([0]*200))
            for args in ([],[str(pcm),'17',str(Q),'400','64','500','1'],
                         [str(pcm),'16','0','400','64','500','1'],
                         [str(pcm),'16',str(Q),'400','0','500','1']):
                r=subprocess.run([binary,*args],capture_output=True)
                self.assertNotEqual(r.returncode,0)
                self.assertEqual(r.stdout,b'')


if __name__=='__main__':
    unittest.main()
