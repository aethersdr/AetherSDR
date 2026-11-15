#!/usr/bin/env python3
"""Real signed successive worker transactions in disposable namespaces."""
import argparse
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
import bundle
import release
import test_management
import test_upgrade_worker
import upgrade

SUCCESSOR=None


class GenerationWorkerTest(test_upgrade_worker.UpgradeWorkerTest):
    def setUp(self):
        test_management.ManagementTest.generation_selection=True
        try:
            super().setUp()
        finally:
            test_management.ManagementTest.generation_selection=False
        self.generation=self.store/'upgrade-generations'/bundle.blake(self.fixture.approved).hex()
        self.generation.mkdir(parents=True,mode=0o700)
        (self.store/'upgrade-generations').chmod(0o700)
        self.stage.rename(self.generation/'pair');self.control.rename(self.generation/'control')
        self.stage=self.generation/'pair';self.control=self.generation/'control'
        intent=(self.capsule/'intent').read_bytes()
        origin=b'PDORIG01'+bundle.blake(self.fixture.approved)+bytes(364)+self.fixture.artifacts[0][:release.PREFIX]
        self.put(self.capsule/'intent',b'PDUPCAP2'+intent[8:728]+origin)

    def finished(self,choice):
        super().finished(choice)
        self.assertEqual((self.store/'upgrade-generation').read_bytes(),b'PDGEN001'+bundle.blake(self.fixture.approved))

    def retained_management_origin(self):
        super().retained_management_origin()
        f=self.fixture
        self.put(self.store/'upgrade-transaction',b'PDUTXN01'+f.retained_approval+struct.pack('>I',7))
        stage=self.store/'upgrade-pair';stage.mkdir(mode=0o700)
        control=self.store/'upgrade-control';control.mkdir(mode=0o700)
        self.put(stage/'approved',f.retained_approval)
        for name,artifact,files in [('previous',f.artifacts[0],f.files[0]),('candidate',f.retained_artifact,f.retained_files)]:
            directory=stage/name;directory.mkdir(mode=0o700)
            self.put(directory/'release.header',artifact[:release.PREFIX])
            for leaf,data in files.items():self.put(directory/leaf,data,0o755 if leaf in ['pd-boot','pd-update'] else 0o644)
        self.put(control/'lock',b'')
        self.put(control/'active',b'PDPAIR01'+bytes(8)+bundle.blake(f.retained_approval)+bundle.blake(f.artifacts[0]))

    def test_generation_candidate_refuses_legacy_capsule(self):
        raw=(self.capsule/'intent').read_bytes();self.put(self.capsule/'intent',b'PDUPCAP1'+raw[8:728])
        self.invoke(expected=3);self.assertNotIn('quiesce:before',self.events())

    def test_prepublication_crashes_leave_original_selection_untouched(self):
        for point in ['upgrade-intent-durable','upgrade-capsule-durable','upgrade-recovery-armed']:
            with self.subTest(point=point):
                self.setUp();self.unprepare();before=(self.store/'active').read_bytes()
                self.invoke(bootstrap=True,fault='kill:'+point,expected=137)
                self.assertFalse(self.capsule.exists());self.assertFalse((self.store/'upgrade-generation').exists())
                self.assertFalse((self.store/'upgrade-transaction').exists())
                self.assertFalse((self.store/'upgrade-control').exists());self.assertFalse((self.store/'upgrade-pair').exists())
                self.assertEqual(before,(self.store/'active').read_bytes())
                self.assertEqual((self.root/'fixture/running').read_bytes(),b'1')
                self.invoke(bootstrap=True);self.invoke();self.finished(1)

    def test_cleanup_guard_failure_does_not_arm_or_stop(self):
        self.unprepare();self.invoke(bootstrap=True,fault='fail:bootstrap:cleanup-idle',expected=4)
        self.assertFalse(self.capsule.exists());self.assertNotIn('quiesce:before',self.events())
        self.assertFalse((self.units/'aetherpd-upgrade-recover.service').exists())
        self.invoke(bootstrap=True);self.invoke();self.finished(1)

    def test_generation_selector_crashes_restore(self):
        for point in ['generation-file','generation-rename','generation-durable']:
            with self.subTest(point=point):
                self.setUp();self.invoke(fault='kill:pair:'+point+':0',expected=137)
                self.invoke(recovery=True);self.finished(0)

    def next_upgrade(self,committed):
        self.unprepare();self.invoke(bootstrap=True)
        self.invoke(recovery=not committed);self.finished(int(committed))
        self.prepare_successor(committed,2,SUCCESSOR)

    def prepare_successor(self,committed,index,worker):
        old=self.fixture;selected=int(committed)
        test_management.ManagementTest.generation_selection=True
        test_management.ManagementTest.candidate_index=index
        extra=test_management.ManagementTest();extra.setUp();self.addCleanup(extra.doCleanups)
        test_management.ManagementTest.generation_selection=False
        test_management.ManagementTest.candidate_index=1
        old.artifacts=[old.artifacts[selected],extra.artifacts[1]]
        old.files=[old.files[selected],extra.files[1]];old.keys=[old.keys[selected],extra.keys[1]]
        old.approved=upgrade.encode(1,'4.3.16.42034','fixture-8600',*map(bundle.blake,old.artifacts),*old.keys)
        self.generation=self.store/'upgrade-generations'/bundle.blake(old.approved).hex()
        self.stage=self.generation/'pair';self.control=self.generation/'control'
        self.put(self.root/'fixture/copy',worker.read_bytes(),0o700)
        self.put(self.root/'fixture/approved',old.approved)
        for i,name in enumerate(['previous','candidate']):self.put(self.root/('fixture/'+name+'.release'),old.artifacts[i])
        self.put(self.root/'fixture/running',b'1');self.put(self.root/'fixture/events',b'')

    def test_successor_after_committed_generation(self):
        self.next_upgrade(True);self.invoke(bootstrap=True);self.invoke();self.finished(1)

    def test_successor_after_rolled_back_generation(self):
        self.next_upgrade(False);self.invoke(bootstrap=True);self.invoke();self.finished(1)

    def test_admission_budget_covers_observed_retirement_transition(self):
        measured=[]
        for successor in [False,True]:
            with self.subTest(successor=successor):
                self.setUp()
                if successor:self.next_upgrade(True)
                else:self.unprepare()
                roots=[self.store.parent,self.units]
                def allocated():
                    paths=[p for root in roots for p in [root,*root.rglob('*')]]
                    return sum(p.lstat().st_blocks*512 for p in paths),len(paths)
                before=allocated();self.put(self.root/'fixture/measure-space',b'1')
                self.invoke(bootstrap=True);self.invoke();self.finished(1)
                observations=[tuple(map(int,line.split())) for line in (self.root/'fixture/space-observations').read_text().splitlines()]
                budgets=[tuple(map(int,line.split())) for line in (self.root/'fixture/space-budget').read_text().splitlines()]
                self.assertEqual(len(budgets),1)
                self.assertLessEqual(max(n[0] for n in observations)-before[0]+8*1024*1024,budgets[0][0])
                self.assertLessEqual(max(n[1] for n in observations)-before[1],budgets[0][1])
                self.assertTrue((self.store/'slots-retired').exists());self.assert_compacted(1)
                measured.append({'successor':successor,'baseline_bytes':before[0],'baseline_inodes':before[1],
                    'peak_bytes':max(n[0] for n in observations),'peak_inodes':max(n[1] for n in observations),
                    'admitted_additional_bytes_including_reserve':budgets[0][0],'admitted_additional_inodes':budgets[0][1]})
        if os.environ.get('AETHERPD_TEST_SPACE_REPORT'):
            Path(os.environ['AETHERPD_TEST_SPACE_REPORT']).write_text(json.dumps(measured,indent=2)+'\n')

    def test_persistent_release_inputs_refuse_before_staging(self):
        root=Path(os.environ.get('AETHERPD_TEST_PERSISTENT_ROOT',str(Path.cwd())))
        filesystem=subprocess.check_output(['stat','-f','-c','%T',str(root)],text=True).strip()
        if filesystem=='tmpfs':self.skipTest('set AETHERPD_TEST_PERSISTENT_ROOT to a writable persistent filesystem')
        self.unprepare()
        self.persistent_inputs=Path(self.enterContext(tempfile.TemporaryDirectory(prefix='pd-input-test-',dir=root)))
        for name in ['previous.release','candidate.release']:
            self.put(self.persistent_inputs/name,(self.root/'fixture'/name).read_bytes())
        self.invoke(bootstrap=True,expected=3)
        self.assertFalse(self.stage.exists());self.assertFalse(self.capsule.exists())
        self.assertNotIn('quiesce:before',self.events());self.assertEqual((self.root/'fixture/running').read_bytes(),b'1')

    def test_successor_retires_exact_predecessor(self):
        for before in [False,True]:
            for after in [False,True]:
                with self.subTest(before=before,after=after):
                    self.setUp();old=self.generation;oldrecord=self.store/'upgrade-retained'/old.name
                    self.next_upgrade(before);self.invoke(bootstrap=True);self.invoke(recovery=not after)
                    self.finished(int(after));self.assertFalse(old.exists());self.assertFalse(oldrecord.exists())
                    self.assert_compacted(int(after))

    def test_successor_retirement_crashes_resume(self):
        points=['retire-bytes','retire-file','retire-rename','pair:retire-durable:0']
        points += ['pair:retire-file:'+str(i) for i in range(23)]
        points += ['pair:retire-directory:'+str(i) for i in range(6)]
        for point in points:
            with self.subTest(point=point):
                self.setUp();old=self.generation;self.next_upgrade(True);self.invoke(bootstrap=True)
                self.invoke(fault='kill:'+point,expected=137);self.invoke(recovery=True)
                self.finished(1);self.assertFalse(old.exists());self.assert_compacted(1)

    def assert_compacted(self,choice):
        selected=self.stage/('candidate' if choice else 'previous')
        obsolete=self.stage/('previous' if choice else 'candidate')
        self.assertEqual((selected/'payload.bundle').read_bytes(),self.fixture.files[choice]['payload.bundle'])
        self.assertFalse((obsolete/'payload.bundle').exists())
        receipt=self.store/'upgrade-retained'/bundle.blake(self.fixture.approved).hex()/'receipt'
        raw=receipt.read_bytes();self.assertEqual(len(raw),1236)
        self.assertEqual(raw[:8],b'PDKEEP01')
        self.assertEqual(raw[8:372],(self.store/'upgrade-transaction').read_bytes())

    def test_terminal_worker_compacts_only_unselected_payload(self):
        for choice in [0,1]:
            with self.subTest(choice=choice):
                self.setUp();self.invoke(recovery=not choice);self.finished(choice);self.assert_compacted(choice)

    def test_original_slot_retirement_runs_for_both_terminal_choices(self):
        for choice in [0,1]:
            with self.subTest(choice=choice):
                self.setUp();self.invoke(recovery=not choice);self.finished(choice)
                self.assertEqual(list((self.store/'slots').iterdir()),[])
                self.assertEqual((self.store/'slots-retired').stat().st_size,652)

    def test_original_slot_retirement_worker_crashes_resume(self):
        points=['slots-retired-bytes','slots-retired-file','slots-retired-rename',
                'pair:slots-durable:0','pair:slots-removed:0','pair:slots-removed:1','pair:slots-removed:2']
        for point in points:
            with self.subTest(point=point):
                self.setUp();self.invoke(fault='kill:'+point,expected=137);self.invoke(recovery=True)
                self.finished(1);self.assertEqual(list((self.store/'slots').iterdir()),[])

    def test_compaction_crashes_resume_terminal_cleanup(self):
        for choice in [0,1]:
            for point in ['receipt-bytes','receipt-file','receipt-rename',
                          *['pair:'+name+':'+str(choice) for name in
                            ['retained-durable','retained-before-unlink','retained-unlinked','retained-complete']]]:
                with self.subTest(choice=choice,point=point):
                    self.setUp();self.invoke(recovery=not choice,fault='kill:'+point,expected=137)
                    self.invoke(recovery=True);self.finished(choice);self.assert_compacted(choice)

    def test_compaction_refusal_still_removes_temporary_authority(self):
        receipt=self.store/'upgrade-retained'/bundle.blake(self.fixture.approved).hex()
        receipt.mkdir(parents=True,mode=0o700);receipt.parent.chmod(0o700)
        self.put(receipt/'unknown',b'preserve')
        self.invoke();self.finished(1)
        self.assertTrue((self.stage/'previous/payload.bundle').exists())
        self.assertEqual((receipt/'unknown').read_bytes(),b'preserve')

    def test_successor_prejournal_recovery_restores_selected_predecessor(self):
        for committed in [True,False]:
            with self.subTest(committed=committed):
                self.setUp();self.next_upgrade(committed);self.invoke(bootstrap=True)
                self.invoke(recovery=True);self.finished(0)

    def test_successor_recovery_after_management_and_selection(self):
        for committed in [True,False]:
            for point in ['quiesce:after','management:rename:0','pair:generation-rename:0','pair:pair-committed:1']:
                with self.subTest(committed=committed,point=point):
                    self.setUp();self.next_upgrade(committed);self.invoke(bootstrap=True)
                    self.invoke(fault='kill:'+point,expected=137);self.invoke(recovery=True);self.finished(0)

    def test_successor_changed_predecessor_refuses_before_quiescence(self):
        self.next_upgrade(False);self.invoke(bootstrap=True)
        path=self.store/'upgrade-transaction';raw=bytearray(path.read_bytes());raw[-1]=4;self.put(path,raw)
        self.invoke(expected=3);self.assertNotIn('quiesce:before',self.events())

    def test_successor_matching_payload_but_foreign_predecessor_refuses(self):
        self.next_upgrade(False);self.invoke(bootstrap=True)
        path=self.store/'upgrade-transaction';raw=bytearray(path.read_bytes())
        raw[8+8+140+16]^=1
        self.put(path,raw);self.invoke(expected=3)
        self.assertNotIn('quiesce:before',self.events());self.assertEqual(path.read_bytes(),raw)

    def test_successor_missing_predecessor_refuses(self):
        self.next_upgrade(False);self.invoke(bootstrap=True)
        (self.store/'upgrade-transaction').unlink();self.invoke(expected=3)
        self.assertNotIn('quiesce:before',self.events())


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--checker',type=Path,required=True)
    p.add_argument('--successor-checker',type=Path,required=True)
    a,rest=p.parse_known_args();test_upgrade_worker.CHECKER=a.checker.resolve();SUCCESSOR=a.successor_checker.resolve()
    unittest.main(argv=[__file__,*rest])
