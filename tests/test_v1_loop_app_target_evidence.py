import unittest
from tools.v1_loop_app_target_evidence import compare,BEGIN,END,RESULT,IDENTITY,CONCURRENT

class AppTargetEvidenceTests(unittest.TestCase):
    def logs(self):
        body=(IDENTITY+'\nAPP_TARGET_TRACE frames=1024 hash=3502127162542228268 events=20 commits=4\n'+
              CONCURRENT+'\nAPP_TARGET_LAYOUT pointer=8 fixture=200000 workspace=6000 channel=1000 runtime=168\n'+RESULT+'\n')
        return body,BEGIN+'\n'+body.replace('pointer=8','pointer=4')+END+'\nI (999) main_task: Returned from app_main()\n'
    def test_complete(self):
        a,b=self.logs();r=compare(a,b);self.assertTrue(r['all_equal']);self.assertFalse(r['runtime_approved'])
    def test_missing_and_duplicate_markers(self):
        a,b=self.logs()
        for marker in (BEGIN,END,RESULT,IDENTITY,CONCURRENT):
            for text in (b.replace(marker,''),b+'\n'+marker):
                with self.subTest(marker=marker),self.assertRaises(ValueError):compare(a,text)
    def test_failure_after_pass_rejected(self):
        a,b=self.logs()
        for marker in ('V1_APP_FAIL','Guru Meditation','task_wdt:','assert failed','Stack canary watchpoint'):
            with self.subTest(marker=marker),self.assertRaises(ValueError):compare(a,b+marker)
    def test_trace_and_asset_mutation_rejected(self):
        a,b=self.logs()
        for old,new in (('frames=1024','frames=1023'),('events=20','events=19'),('commits=4','commits=3'),
                        ('3502127162542228268','3502127162542228269'),('e1c3af71','e1c3af72'),('pointer=4','pointer=8')):
            with self.subTest(old=old),self.assertRaises(ValueError):compare(a,b.replace(old,new))
    def test_return_and_order_required(self):
        a,b=self.logs()
        for text in (b.replace('Returned from app_main()',''),b+'Returned from app_main()',
                     b.replace(BEGIN,'')+BEGIN,b.replace(END,'')+END):
            with self.assertRaises(ValueError):compare(a,text)
    def test_native_cannot_silently_drift(self):
        a,b=self.logs()
        with self.assertRaises(ValueError):compare(a.replace('events=20','events=21'),b.replace('events=20','events=21'))
    def test_crlf(self):
        a,b=self.logs();self.assertTrue(compare(a.replace('\n','\r\n'),b.replace('\n','\r\n'))['all_equal'])
if __name__=='__main__':unittest.main()
