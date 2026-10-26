import array
import unittest
from tools.v1_loop_prepared_check import reference as raw
from tools.v1_loop_voice_check import reference
from tools.v1_loop_release_check import Q, pack

class VoiceOracleTests(unittest.TestCase):
    def test_no_noteoff_is_frozen_transport(self):
        pcm=[(i*997)%60001-30000 for i in range(603)]
        for fade in (0,16,32,64):
            self.assertEqual(reference(pcm,23,523,fade,Q,1700,1701,0,1),
                             raw(pcm,23,523,fade,Q,1700,1701,0,1))
    def test_q15_release_and_count(self):
        pcm=[32767]*603
        values=array.array('h');values.frombytes(reference(pcm,23,523,32,Q,1700,501,32,1))
        self.assertEqual(len(values),757)
        self.assertEqual(values[500],32767)
        self.assertEqual(values[501],32767)
        self.assertEqual(values[502],(32767*(32767*255//256)+16384)//32768)
        self.assertEqual(values[-1],127)
    def test_sign_symmetry(self):
        pos=reference([20000]*603,23,523,32,Q,1700,501,32,1)
        neg=reference([-20000]*603,23,523,32,Q,1700,501,32,1)
        a=array.array('h');a.frombytes(pos);b=array.array('h');b.frombytes(neg)
        self.assertEqual(list(a),[-x for x in b])
    def test_natural_end_can_precede_envelope(self):
        actual=reference([12000]*527,23,523,32,Q,1700,522,32,3)
        self.assertEqual(len(actual)//2,527)
if __name__=='__main__':unittest.main()
