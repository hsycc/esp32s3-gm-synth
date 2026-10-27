import copy
import unittest
from tools.v1_loop_analysis_evidence import compare as logs, EXPECTED, DIGEST
from tools.v1_loop_analysis_check import compare
from tools.v1_loop_policy import analyze

class AnalysisEvidenceTests(unittest.TestCase):
    def fixture(self):
        result='ANALYSIS_RESULT suites={} frames={} trace={} initial_fade={} replacement_fade={} work={} reads={}\n'.format(*EXPECTED)
        common=result+'ANALYSIS_LAYOUT pointer=4 analyzer=4096 provider=4600 fixture=253840\n'+\
            'ANALYSIS_ASSET sha256='+DIGEST+'\nANALYSIS_PASS\n'
        return common.replace('pointer=4','pointer=8'),'ANALYSIS_BEGIN esp32s3 macro=1 resident_analysis=1\n'+common+'ANALYSIS_END\nReturned from app_main()\n'
    def test_complete_target_execution(self):
        a,b=self.fixture();r=logs(a,b)
        self.assertFalse(r['runtime_approved']);self.assertEqual(r['initial_fade'],32)
    def test_missing_or_duplicate(self):
        a,b=self.fixture()
        for text in (b+b,b.replace('ANALYSIS_PASS',''),b.replace('Returned from app_main()',''),b.replace('ANALYSIS_END','')):
            with self.assertRaises(ValueError):logs(a,text)
    def test_wrong_trace_or_asset(self):
        a,b=self.fixture()
        for text in (b.replace('initial_fade=32','initial_fade=16'),b.replace(DIGEST,'0'*64),b.replace('pointer=4','pointer=8')):
            with self.assertRaises(ValueError):logs(a,text)
    def test_late_fatal_or_wrong_order(self):
        a,b=self.fixture()
        for text in (b+'task_wdt:',b+'ANALYSIS_FAIL',b.replace('ANALYSIS_END','')+'ANALYSIS_END'):
            with self.assertRaises(ValueError):logs(a,text)
    def test_comparison_rejects_bad_decision_or_evidence(self):
        expected=analyze([0]*4096,dict(scope='prepared_zone',loop_mode='continuous',sample_type=1,noteon_address_modulated=False))
        bad=dict(selected=16,best=0,runtime_approved=False,candidates=[])
        with self.assertRaises(ValueError):compare(bad,expected)
        bad['selected']=0
        with self.assertRaises(ValueError):compare(bad,expected)
    def test_fatal_native_is_not_a_reference(self):
        a,b=self.fixture()
        with self.assertRaises(ValueError):logs(a+'CORE_FAIL',b)

if __name__=='__main__':unittest.main()
