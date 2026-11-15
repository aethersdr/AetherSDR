#!/usr/bin/env python3
"""Repeated signed updates exercise inherited retirement records and bounded trees."""
import argparse
from pathlib import Path
import unittest
import test_generation_worker
import test_upgrade_worker

WORKERS=[]

class ChainTest(unittest.TestCase):
    def chain(self,choices,fault=None):
        f=test_generation_worker.GenerationWorkerTest();f.setUp();self.addCleanup(f.doCleanups)
        f.unprepare();f.invoke(bootstrap=True);f.invoke(recovery=not choices[0]);f.finished(int(choices[0]))
        self.assertEqual(list((f.store/'slots').iterdir()),[])
        receipt=(f.store/'slots-retired').read_bytes()
        old=f.generation
        for index,choice in enumerate(choices[1:],2):
            with self.subTest(update=index,choice=choice):
                f.prepare_successor(choices[index-2],index,WORKERS[index-2])
                f.invoke(bootstrap=True)
                if fault and index==3:
                    f.invoke(recovery=not choice,fault='kill:'+fault,expected=137)
                    f.invoke(recovery=True)
                else:f.invoke(recovery=not choice)
                f.finished(int(choice));f.assert_compacted(int(choice));self.assertFalse(old.exists())
                self.assertEqual(list((f.store/'slots').iterdir()),[])
                self.assertEqual(receipt,(f.store/'slots-retired').read_bytes())
                self.assertEqual([p.name for p in (f.store/'upgrade-generations').iterdir()],[f.generation.name])
                self.assertEqual([p.name for p in (f.store/'upgrade-retained').iterdir()],[f.generation.name])
                old=f.generation

    def test_four_committed_updates_keep_one_generation_and_receipt(self):
        self.chain([True,True,True,True])

    def test_alternating_commit_and_rollback_keep_one_generation(self):
        self.chain([False,True,False,True]);self.chain([True,False,True,False])

    def test_inherited_removal_record_crash_resumes(self):
        for point in ['pair:retire-file:22','pair:retire-directory:5']:
            with self.subTest(point=point):self.chain([True,True,True,True],point)

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--checker',required=True,type=Path)
    p.add_argument('--successors',required=True,type=Path,nargs=3)
    a,rest=p.parse_known_args();test_upgrade_worker.CHECKER=a.checker.resolve();WORKERS=[p.resolve() for p in a.successors]
    unittest.main(argv=[__file__,*rest])
