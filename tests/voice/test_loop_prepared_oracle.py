import unittest
from tools.v1_loop_prepared_check import reference
from tools.v1_loop_release_check import Q, pack, reference as release_reference

class PreparedOracleTests(unittest.TestCase):
    def test_attack_then_loop(self):
        pcm=list(range(180))
        self.assertEqual(reference(pcm,17,130,0,Q,200,999,0,1),pack(pcm[:130]+pcm[17:87]))

    def test_actual_continuous_ignores_noteoff(self):
        pcm=[i*19-7000 for i in range(600)]
        self.assertEqual(reference(pcm,21,570,32,Q,1300,555,32,1),
                         reference(pcm,21,570,32,Q,1300,9999,0,1))

    def test_release_window_matches_frozen_oracle(self):
        pcm=[((i*731)%40001)-20000 for i in range(640)]
        for step in (Q//2,Q,5*Q//4):
            for release in (0,16,32,64):
                off=(605*Q+step-1)//step
                window,meta=release_reference(pcm,23,610,32,step,off,release,3)
                full=reference(pcm,23,610,32,step,off+129,off,release,3)
                self.assertEqual(full[2*max(0,off-4):],pack(window))
                self.assertEqual(len(window),meta['emitted'])

    def test_short_tail_and_single_frame(self):
        pcm=[((i*711)%65536)-32768 for i in range(640)]
        for off in (638,639):
            full=reference(pcm,0,640,32,Q,800,off,32,3)
            self.assertEqual(len(full),1280)
            self.assertEqual(full[-2:],pack([pcm[-1]]))

    def test_blocks_are_not_an_oracle_input(self):
        # The native block-invariance check must compare against a transport
        # oracle with no block state, rather than sharing native chunk logic.
        import inspect
        self.assertNotIn('block',inspect.signature(reference).parameters)

if __name__=='__main__': unittest.main()
