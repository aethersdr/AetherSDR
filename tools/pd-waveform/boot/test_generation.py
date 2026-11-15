#!/usr/bin/env python3
"""Independent staging and durable generation selection in disposable stores."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import struct
import subprocess
import unittest
import test_upgrade
import units

CHECKER=None

class GenerationTest(unittest.TestCase):
    def setUp(self):
        self.fixture=test_upgrade.UpgradeTest();self.fixture.setUp();self.addCleanup(self.fixture.doCleanups)
        self.root=self.fixture.root;self.store=self.fixture.store
        self.fixture.write(self.store/'store.lock',b'')
        self.origin=self.root/'origin'
    def invoke(self,mode,expected=0,fault='',key=None,fixture=None):
        f=fixture or self.fixture
        result=subprocess.run([str(CHECKER),str(f.root),*map(str,f.paths),str(f.approved),str(key or f.key),
                               mode,str(f.root/'origin'),fault],capture_output=True,text=True,timeout=10)
        self.assertEqual(result.returncode,expected,result.stderr)
        return json.loads(result.stdout) if expected==0 and mode in ['stage','active'] else None
    def stage(self):
        result=self.invoke('stage');self.fixture.write(self.origin,bytes.fromhex(result['origin']))
        digest=hashlib.blake2b(self.fixture.approved.read_bytes(),digest_size=32).hexdigest()
        return self.store/'upgrade-generations'/digest
    def begin(self,phase=1):
        self.fixture.write(self.store/'upgrade-transaction',b'PDUTXN01'+self.fixture.approved.read_bytes()+struct.pack('>I',phase))
        (self.root/'aetherpd-upgrade').mkdir(mode=0o700,exist_ok=True)
    def selected_record(self):
        return b'PDGEN001'+hashlib.blake2b(self.fixture.approved.read_bytes(),digest_size=32).digest()
    def snapshot(self,path):
        result={}
        for p in path.rglob('*'):
            s=p.lstat();result[str(p.relative_to(path))]=(s.st_ino,s.st_mode,os.readlink(p) if p.is_symlink() else p.read_bytes() if p.is_file() else None)
        return result
    def legacy(self,committed=True):
        self.fixture.terminal_predecessor(committed)
        key=self.root/'prior-key';self.fixture.write(key,self.fixture.keys[1]);return key
    def test_staging_does_not_select_or_change_original_installation(self):
        before=self.snapshot(self.store);self.stage()
        after=self.snapshot(self.store)
        self.assertEqual(before,{n:v for n,v in after.items() if not n.startswith('upgrade-generations')})
        self.assertEqual(self.invoke('active')['selected'],0)
    def test_staging_preserves_legacy_commit_selection(self):
        key=self.legacy();before=self.snapshot(self.store/'upgrade-control');self.stage()
        self.assertEqual(before,self.snapshot(self.store/'upgrade-control'))
        self.assertEqual(self.invoke('active',key=key),{'selected':1,'legacy':1,'choice':1})
    def test_staging_preserves_legacy_rollback_selection(self):
        key=self.legacy(False);self.stage()
        self.assertEqual(self.invoke('active',key=key),{'selected':1,'legacy':1,'choice':0})
    def test_matching_staging_retry_is_idempotent(self):
        self.legacy();generation=self.stage();before=self.snapshot(generation)
        self.stage();self.assertEqual(before,self.snapshot(generation))
    def test_partial_generation_staging_resumes_in_fresh_process(self):
        self.legacy();before=self.snapshot(self.store/'upgrade-control')
        self.invoke('stage',expected=-signal.SIGKILL,fault='resume-file-synced:0')
        self.stage();self.assertEqual(before,self.snapshot(self.store/'upgrade-control'))
    def test_original_installation_to_generation_previous_selection(self):
        self.stage();self.begin();self.invoke('select')
        self.assertEqual((self.store/'upgrade-generation').read_bytes(),self.selected_record())
        self.assertEqual(self.invoke('active'),{'selected':1,'legacy':0,'choice':0})
    def test_legacy_terminal_to_generation_previous_selection(self):
        self.legacy();self.stage();self.begin();self.invoke('select')
        self.assertEqual(self.invoke('active'),{'selected':1,'legacy':0,'choice':0})
        before=self.snapshot(self.store);self.invoke('select');self.assertEqual(before,self.snapshot(self.store))
    def test_legacy_rollback_to_generation_previous_selection(self):
        self.legacy(False);self.stage();self.begin(5);self.invoke('select')
        self.assertEqual(self.invoke('active'),{'selected':1,'legacy':0,'choice':0})
    def another_generation(self, committed):
        first=self.stage();self.begin();self.invoke('select')
        if committed:self.invoke('candidate')
        self.begin(4 if committed else 7);(self.root/'aetherpd-upgrade').rmdir()
        for name in ['pd-boot','BOOT-LICENSES.txt']:
            self.fixture.write(self.store/name,self.fixture.files[1][name],0o755 if name=='pd-boot' else 0o644)
        for name in units.render('arm64'):
            for directory in [self.store/'units',self.root/'units']:
                self.fixture.write(directory/name,self.fixture.files[1][name],0o644)
        before=self.snapshot(first);selector=(self.store/'upgrade-generation').read_bytes()
        self.fixture.next_inputs(committed);second=self.stage()
        self.assertNotEqual(first,second);self.assertEqual(before,self.snapshot(first))
        self.assertEqual((self.store/'upgrade-generation').read_bytes(),selector)
        self.begin();self.invoke('select');self.assertEqual(self.invoke('active')['choice'],0)
        self.assertEqual(before,self.snapshot(first))
    def test_successor_after_generation_commit(self):
        self.another_generation(True)
    def test_successor_after_generation_rollback(self):
        self.another_generation(False)
    def test_generation_publication_interruptions_resume(self):
        self.legacy();self.stage();self.begin()
        for point in ['generation-file:0','generation-rename:0','generation-durable:0']:
            (self.store/'upgrade-generation').unlink(missing_ok=True)
            self.invoke('select',expected=-signal.SIGKILL,fault=point)
            self.invoke('select');self.assertEqual(self.invoke('active')['legacy'],0)
    def test_every_matching_selector_prefix_resumes(self):
        self.legacy();self.stage();self.begin();data=self.selected_record()
        for length in range(len(data)+1):
            self.fixture.write(self.store/'upgrade-generation.new',data[:length]);self.invoke('select')
            self.assertFalse((self.store/'upgrade-generation.new').exists())
    def test_foreign_selector_or_partial_refuses_without_changes(self):
        self.legacy();self.stage();self.begin()
        for name in ['upgrade-generation','upgrade-generation.new']:
            path=self.store/name;self.fixture.write(path,b'foreign');before=self.snapshot(self.store)
            self.invoke('select',expected=1);self.assertEqual(before,self.snapshot(self.store));path.unlink()
    def test_selector_requires_live_matching_worker_phase(self):
        self.stage();self.begin()
        for phase in [2,3,4,6,7]:
            self.begin(phase);self.invoke('select',expected=1);self.assertFalse((self.store/'upgrade-generation').exists())
        self.begin();self.invoke('select',expected=1,fault='no-lock')
        (self.root/'aetherpd-upgrade').rmdir();self.invoke('select',expected=1)
    def test_unsafe_generation_directory_is_not_followed(self):
        self.stage();self.begin();root=self.store/'upgrade-generations';preserved=self.root/'preserved';root.rename(preserved);root.symlink_to(preserved)
        before=self.snapshot(preserved);self.invoke('select',expected=1);self.assertEqual(before,self.snapshot(preserved))
    def test_wrong_stage_approval_refuses_before_selector_publication(self):
        self.legacy();generation=self.stage();self.begin()
        other=test_upgrade.UpgradeTest();other.setUp();self.addCleanup(other.doCleanups);other.terminal_predecessor(False)
        result=self.invoke('stage',fixture=other);self.assertTrue(result['origin'])
        digest=hashlib.blake2b(other.approved.read_bytes(),digest_size=32).hexdigest()
        alternate=other.store/'upgrade-generations'/digest
        for name in ['pair','control']:
            shutil.rmtree(generation/name);shutil.copytree(alternate/name,generation/name)
        before=self.snapshot(generation);self.invoke('select',expected=1)
        self.assertFalse((self.store/'upgrade-generation').exists());self.assertEqual(before,self.snapshot(generation))
    def test_active_generation_rejects_journal_mismatch_and_partial(self):
        self.stage();self.begin();self.invoke('select')
        p=self.store/'upgrade-transaction';original=p.read_bytes();data=bytearray(original);data[8+148+16]^=1
        self.fixture.write(p,data);self.invoke('active',expected=1);self.fixture.write(p,original)
        self.fixture.write(self.store/'upgrade-generation.new',b'');self.invoke('active',expected=1)
    def test_missing_generation_selector_cannot_fall_back_to_original(self):
        self.stage();self.begin();self.invoke('select')
        (self.store/'upgrade-generation').unlink();self.invoke('active',expected=1)
    def test_foreign_generation_member_refuses_staging_without_mutation(self):
        generation=self.stage();self.fixture.write(generation/'foreign',b'preserve')
        before=self.snapshot(generation);self.invoke('stage',expected=1);self.assertEqual(before,self.snapshot(generation))
    def test_selector_metadata_and_links_are_rejected(self):
        self.stage();self.begin();self.invoke('select');p=self.store/'upgrade-generation'
        for mode in [0o644,0o4600]:
            p.chmod(mode);self.invoke('active',expected=1);self.invoke('select',expected=1)
        p.chmod(0o600);alias=self.root/'alias';os.link(p,alias);self.invoke('active',expected=1);alias.unlink()
        p.rename(alias);p.symlink_to(alias);self.invoke('active',expected=1);self.invoke('select',expected=1)

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--checker',type=Path,required=True);p.add_argument('--upgrade-checker',type=Path,required=True)
    a,rest=p.parse_known_args();CHECKER=a.checker.resolve();test_upgrade.CHECKER=a.upgrade_checker.resolve()
    unittest.main(argv=[__file__,*rest])
