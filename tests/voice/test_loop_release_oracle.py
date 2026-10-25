import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from tools.v1_loop_release_check import Q,check_geometry,held_phase,pack,point,probe,reference


def fixture():
    seed=731
    pcm=[]
    for _ in range(600):
        seed=(1664525*seed+1013904223)&0xffffffff
        pcm.append(seed%65536-32768)
    return pcm


class ReleaseOracleTests(unittest.TestCase):
    def test_closed_form_phase_after_several_wraps(self):
        self.assertEqual(held_phase(531,Q,19,531,32),51*Q)
        self.assertEqual(held_phase(531+480,Q,19,531,32),51*Q)
        self.assertEqual(held_phase(1061,Q//2,19,531,32),530*Q+Q//2)

    def test_first_is_held_last_is_raw(self):
        pcm=fixture()
        y,m=reference(pcm,19,531,32,Q,510,32)
        raw,_=reference(pcm,19,531,32,Q,510,0)
        held,_=reference(pcm,19,531,32,Q,510,32,1)
        self.assertTrue(m['started'])
        self.assertFalse(m['truncated'])
        self.assertEqual(y[4],held[4])
        self.assertEqual(y[35:],raw[35:])
        self.assertEqual(m['bridge_frames'],32)
        for j in range(32):
            self.assertGreaterEqual(y[4+j],min(raw[4+j],held[4+j]))
            self.assertLessEqual(y[4+j],max(raw[4+j],held[4+j]))

    def test_single_frame_tail_falls_back_without_extension(self):
        pcm=fixture()[:531]
        y,m=reference(pcm,19,531,32,Q,530,32)
        self.assertEqual(y[-1],pcm[530])
        self.assertEqual(m['emitted'],5)
        self.assertEqual(m['bridge_frames'],1)
        self.assertTrue(m['truncated'])
        self.assertFalse(m['active'])

    def test_short_tail_has_a_complete_shorter_ramp(self):
        pcm=fixture()[:539]
        y,m=reference(pcm,19,531,32,Q,530,32)
        raw,_=reference(pcm,19,531,32,Q,530,0)
        held,_=reference(pcm,19,531,32,Q,530,32,1)
        self.assertEqual(m['planned_frames'],9)
        self.assertEqual(m['bridge_frames'],9)
        self.assertTrue(m['tail_limited'])
        self.assertFalse(m['truncated'])
        self.assertEqual(y[4],held[4])
        self.assertEqual(y[-1],raw[-1])
        for j in range(9):
            n=held[4+j]*(8-j)+raw[4+j]*j
            expected=(abs(n)+4)//8*(1 if n>=0 else -1)
            self.assertEqual(y[4+j],expected)

    def test_direct_and_continuous_not_changed(self):
        pcm=fixture()
        for fade,mode in ((0,3),(0,1),(32,1)):
            a,_=reference(pcm,19,531,fade,Q,510,0,mode)
            b,m=reference(pcm,19,531,fade,Q,510,32,mode)
            self.assertEqual(a,b)
            self.assertFalse(m['started'])

    def test_constant_extreme_needs_no_transition(self):
        for v in (-32768,32767):
            values,meta=reference([v]*600,19,531,32,Q,510,64)
            self.assertEqual(set(values),{v})
            self.assertFalse(meta['started'])

    def test_invalid_configurations(self):
        pcm=fixture()
        for ls,le,fade,step,off,duration,mode in (
                (19,531,8,Q,510,32,3),(19,531,32,0,510,32,3),
                (19,531,32,Q,510,33,3),(19,601,32,Q,510,32,3),
                (19,531,32,Q,-1,32,3),(19,531,32,Q,510,32,2)):
            with self.assertRaises(ValueError):
                reference(pcm,ls,le,fade,step,off,duration,mode)


@unittest.skipUnless(os.environ.get('V1_RELEASE_PROBE'),'native release probe required in dedicated CI')
class NativeReleaseTests(unittest.TestCase):
    def test_native_oracle_and_block_invariance(self):
        binary=Path(os.environ['V1_RELEASE_PROBE'])
        for mode in (1,3):
            for fade in (0,16,32,64):
                rows=check_geometry(binary,fixture(),19,531,fade,mode)
                self.assertEqual(len(rows),14)
                self.assertTrue(all(c['equal'] for c in rows))

    def test_probe_rejects_bad_config(self):
        binary=Path(os.environ['V1_RELEASE_PROBE'])
        with tempfile.TemporaryDirectory() as td:
            path=Path(td)/'pcm'; path.write_bytes(pack(fixture()))
            for duration in (-1,1,65):
                with self.assertRaises(subprocess.CalledProcessError):
                    probe(binary,path,19,531,32,Q,510,duration,64,3)
            path.write_bytes(b'odd')
            with self.assertRaises(subprocess.CalledProcessError):
                probe(binary,path,19,531,32,Q,510,32,64,3)


if __name__=='__main__':
    unittest.main()
