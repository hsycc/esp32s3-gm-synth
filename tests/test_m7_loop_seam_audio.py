"""Pure host tests for the normalized-loop audition renderer."""
import sys
import tempfile
import unittest
import wave
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from m7_loop_seam_audio import render_loop, measures, save_wave

class TestLoopSeamAB(unittest.TestCase):
    def test_direct_loop(self):
        pcm=[0,0,100,200,1000,1000]
        samples,wraps=render_loop(pcm,0,20)
        self.assertEqual(wraps[0],6)
        self.assertEqual(samples[:8],pcm+pcm[:2])
        self.assertEqual(measures(samples,wraps,0)["wrap_step_pcm16"],1000)

    def test_crossfade_reduces_synthetic_wrap(self):
        pcm=[0]*20+[20000]*20
        a,aw=render_loop(pcm,0,100)
        b,bw=render_loop(pcm,8,100)
        self.assertLess(measures(b,bw,8)["wrap_step_pcm16"],
                        measures(a,aw,0)["wrap_step_pcm16"])
        self.assertEqual(bw[0],40)
        self.assertEqual(b[40],pcm[8])

    def test_reject_out_of_range(self):
        with self.assertRaises(ValueError):
            render_loop([0,1,2,3],2,10)
        with self.assertRaises(ValueError):
            render_loop([0,1,2,3],0,3)

    def test_wav_mono_header(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d)/"test.wav"
            save_wave(p,[0,-32768,32767],44100)
            with wave.open(str(p),"rb") as f:
                self.assertEqual((f.getnchannels(),f.getsampwidth(),
                                  f.getframerate(),f.getnframes()),
                                 (1,2,44100,3))

if __name__=="__main__":
    unittest.main()
