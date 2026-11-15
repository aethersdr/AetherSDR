#!/usr/bin/env python3
"""Exact predecessor retirement with durable bounded deletion recovery."""
import argparse
import fcntl
import os
from pathlib import Path
import signal
import shutil
import unittest
import bundle
import test_generation
import test_upgrade
import units


class RetireTest(test_generation.GenerationTest):
    def prepare(self,prior_commit=True,current_commit=True,legacy=False,compact=True):
        if legacy:
            self.legacy(prior_commit)
            self.old=None;self.oldstage=self.store/'upgrade-pair';self.oldcontrol=self.store/'upgrade-control'
            oldapproval=(self.oldstage/'approved').read_bytes()
        else:
            self.old=self.stage();self.begin();self.invoke('select')
            if prior_commit:self.invoke('candidate')
            self.begin(4 if prior_commit else 7)
            if compact:self.invoke('compact')
            self.oldstage=self.old/'pair';self.oldcontrol=self.old/'control'
            oldapproval=self.fixture.approved.read_bytes()
            (self.root/'aetherpd-upgrade').rmdir()
            for name in ['pd-boot','BOOT-LICENSES.txt']:
                self.fixture.write(self.store/name,self.fixture.files[1][name],0o755 if name=='pd-boot' else 0o644)
            for name in units.render('arm64'):
                for directory in [self.store/'units',self.root/'units']:
                    self.fixture.write(directory/name,self.fixture.files[1][name],0o644)
            self.fixture.next_inputs(prior_commit)
        self.oldrecord=self.store/'upgrade-retained'/bundle.blake(oldapproval).hex()
        self.current=self.stage();self.begin();self.invoke('select')
        if current_commit:self.invoke('candidate')
        self.begin(4 if current_commit else 7);self.invoke('compact')
        self.currentrecord=self.store/'upgrade-retained'/bundle.blake(self.fixture.approved.read_bytes()).hex()
        self.selected=self.current/'pair'/('candidate' if current_commit else 'previous')
        self.private=self.store/'private';self.private.mkdir(mode=0o700)
        self.fixture.write(self.private/'identity',b'fixture private identity')

    def assert_retired(self):
        self.assertFalse(self.oldstage.exists());self.assertFalse(self.oldcontrol.exists())
        if self.old:self.assertFalse(self.old.exists())
        self.assertFalse(self.oldrecord.exists())
        self.invoke('active');self.invoke('retained-read')

    def test_exact_predecessor_removed_for_all_terminal_selections(self):
        for legacy in [False,True]:
            for before in [False,True]:
                for after in [False,True]:
                    with self.subTest(legacy=legacy,before=before,after=after):
                        self.setUp();self.prepare(before,after,legacy)
                        snapshot=self.snapshot(self.current);identity=self.snapshot(self.private)
                        self.invoke('retire');self.assert_retired()
                        self.assertEqual(snapshot,self.snapshot(self.current));self.assertEqual(identity,self.snapshot(self.private))
                        snapshot=self.snapshot(self.store);self.invoke('retire');self.assertEqual(snapshot,self.snapshot(self.store))

    def test_uncompacted_predecessor_is_supported(self):
        self.prepare(compact=False);self.invoke('retire');self.assert_retired()

    def test_crashes_resume_without_touching_selected_release(self):
        points=['retire-bytes','retire-file','retire-rename','retire-durable:0']
        points += ['retire-file:'+str(i) for i in range(23)]
        points += ['retire-directory:'+str(i) for i in range(6)]
        for legacy in [False,True]:
            for point in points:
                if legacy and point=='retire-directory:4':continue
                if legacy and point in ['retire-file:21','retire-file:22']:continue
                with self.subTest(legacy=legacy,point=point):
                    self.setUp();self.prepare(legacy=legacy);snapshot=self.snapshot(self.current)
                    self.invoke('retire',expected=-signal.SIGKILL,fault=point)
                    self.invoke('active');self.assertEqual(snapshot,self.snapshot(self.current))
                    self.invoke('retire');self.assert_retired()

    def test_old_pair_reader_prevents_retirement(self):
        self.prepare();fd=os.open(self.oldcontrol/'lock',os.O_RDONLY)
        try:
            fcntl.flock(fd,fcntl.LOCK_SH|fcntl.LOCK_NB);before=self.snapshot(self.store)
            self.invoke('retire',expected=1);self.assertEqual(before,self.snapshot(self.store))
        finally:os.close(fd)
        self.invoke('retire');self.assert_retired()

    def test_foreign_entries_and_changed_members_are_preserved(self):
        self.prepare()
        for directory in [self.old,self.oldstage,self.oldcontrol,self.oldstage/'previous',self.oldrecord]:
            path=directory/'unknown';self.fixture.write(path,b'preserve');before=self.snapshot(self.store)
            self.invoke('retire',expected=1);self.assertEqual(before,self.snapshot(self.store));path.unlink()
        path=self.oldstage/'candidate/pd-boot';original=path.read_bytes();path.write_bytes(b'foreign')
        before=self.snapshot(self.store);self.invoke('retire',expected=1);self.assertEqual(before,self.snapshot(self.store));path.write_bytes(original)
        self.invoke('retire');self.assert_retired()

    def test_partial_deletion_refuses_replaced_file(self):
        self.prepare();self.invoke('retire',expected=-signal.SIGKILL,fault='retire-file:0')
        path=self.oldstage/'candidate/pd-boot';path.write_bytes(b'foreign');before=self.snapshot(self.store)
        self.invoke('retire',expected=1);self.assertEqual(before,self.snapshot(self.store))

    def test_wrong_manifest_or_partial_prefix_is_preserved(self):
        self.prepare();self.invoke('retire',expected=-signal.SIGKILL,fault='retire-durable:0')
        path=self.currentrecord/'retire';data=path.read_bytes()
        for offset in [0,8,16,100,852,900]:
            changed=bytearray(data);changed[offset]^=1;path.write_bytes(changed);before=self.snapshot(self.store)
            self.invoke('retire',expected=1);self.assertEqual(before,self.snapshot(self.store))
        path.unlink();self.fixture.write(self.currentrecord/'retire.new',b'foreign');before=self.snapshot(self.store)
        self.invoke('retire',expected=1);self.assertEqual(before,self.snapshot(self.store))

    def test_phase_authority_and_current_selection_guards(self):
        self.prepare();before=self.snapshot(self.store);self.invoke('retire',expected=1,fault='no-lock')
        self.assertEqual(before,self.snapshot(self.store))
        self.begin(3);self.invoke('retire',expected=1);self.begin(4)
        self.fixture.write(self.current/'control/active.new',b'');self.invoke('retire',expected=1)
        (self.current/'control/active.new').unlink();(self.root/'aetherpd-upgrade').rmdir();self.invoke('retire',expected=1)
        self.assertTrue(self.old.exists())

    def test_incomplete_retirement_prevents_successor_admission(self):
        self.prepare();self.invoke('retire',expected=-signal.SIGKILL,fault='retire-file:0')
        self.invoke('retire-ready',expected=1);self.invoke('retire');self.invoke('retire-ready')

    def test_unrelated_generations_are_preserved(self):
        self.prepare();other=self.store/'upgrade-generations'/('a'*64);other.mkdir(mode=0o700)
        self.fixture.write(other/'unknown',b'unrelated payload');before=self.snapshot(other)
        self.invoke('retire');self.assertEqual(before,self.snapshot(other));self.assert_retired()

    def test_receipt_metadata_and_directory_aliases_refuse(self):
        self.prepare();receipt=self.oldrecord/'receipt';raw=receipt.read_bytes()
        changed=bytearray(raw);changed[-1]^=1;receipt.write_bytes(changed)
        self.invoke('retire',expected=1);receipt.write_bytes(raw)
        alias=self.root/'alias';os.link(receipt,alias);self.invoke('retire',expected=1);alias.unlink()
        self.old.rename(alias);self.old.symlink_to(alias);before=self.snapshot(alias)
        self.invoke('retire',expected=1);self.assertEqual(before,self.snapshot(alias))

    def test_missing_lock_domain_during_partial_retirement_refuses(self):
        self.prepare();self.invoke('retire',expected=-signal.SIGKILL,fault='retire-durable:0')
        shutil.rmtree(self.oldcontrol);before=self.snapshot(self.store)
        self.invoke('retire',expected=1);self.assertEqual(before,self.snapshot(self.store))


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--checker',required=True,type=Path)
    p.add_argument('--upgrade-checker',required=True,type=Path)
    a,rest=p.parse_known_args();test_generation.CHECKER=a.checker.resolve();test_upgrade.CHECKER=a.upgrade_checker.resolve()
    unittest.main(argv=[__file__,*rest])
