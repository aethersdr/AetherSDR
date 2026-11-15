#!/usr/bin/env python3
"""Fixed migration begin operation with injected predecessor lifecycle; no sockets."""
import argparse
import fcntl
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
import approval
import migration

CHECKER=None


class MigrationTest(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(prefix='pd-migration-');self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name);self.store=self.root/'store';self.store.mkdir(mode=0o755)
        self.state=self.store/'state';self.state.mkdir(mode=0o700)
        self.source=self.root/'source';self.source.mkdir(mode=0o700);(self.source/'tsnet').mkdir(mode=0o700)
        self.config=b'before-stop';self.identity=b'synthetic-identity'
        for path,data in [(self.source/'provision.json',self.config),(self.source/'tsnet/tailscaled.state',self.identity)]:
            path.write_bytes(data);path.chmod(0o600)
        self.target=approval.encode(1,'4.3.16.42034','fixture-8600',bytes(range(32)))
        self.raw=migration.encode(self.target,'a'*64,'b'*64,bytes(range(32)))
        self.record=self.root/'approved';self.record.write_bytes(self.raw);self.record.chmod(0o600)
        self.trace=self.root/'trace'

    def invoke(self,fault='',expected=0,recover=False,bind=False):
        p=subprocess.run([str(CHECKER),str(self.store),str(self.record),str(self.source),str(self.trace),fault]+(['bind'] if bind else ['recover'] if recover else []),capture_output=True,timeout=10)
        self.assertEqual(p.returncode,expected,p.stderr.decode(errors='replace'))
        return self.trace.read_text().splitlines()

    def test_bootstrap_plan_binding_has_no_predecessor_actions(self):
        self.assertEqual(self.invoke(bind=True),[])
        self.assertEqual((self.store/'migration-approved').read_bytes(),self.raw)
        self.assertEqual(self.invoke(bind=True),[])
        self.assertFalse((self.store/'recovery-recipe').exists())
        self.assertEqual(list(self.state.iterdir()),[])

    def test_bootstrap_plan_binding_preserves_foreign_approval(self):
        path=self.store/'migration-approved';path.write_bytes(b'foreign');path.chmod(0o600)
        self.assertEqual(self.invoke(bind=True,expected=6),[])
        self.assertEqual(path.read_bytes(),b'foreign')

    def test_bootstrap_plan_binding_resumes_partial_publication(self):
        path=self.store/'migration-approved.new';path.write_bytes(self.raw[:100]);path.chmod(0o600)
        self.assertEqual(self.invoke(bind=True),[])
        self.assertFalse(path.exists());self.assertEqual((self.store/'migration-approved').read_bytes(),self.raw)

    def test_copy_leaves_predecessor_stopped(self):
        events=self.invoke();self.assertEqual(events,['probe','pin','stop','stopped'])
        self.assertEqual((self.state/'provision.json').read_bytes(),self.config)
        self.assertEqual((self.state/'tsnet/tailscaled.state').read_bytes(),self.identity)
        self.assertEqual((self.store/'migration-approved').read_bytes(),self.raw)
        self.assertEqual((self.store/'migration-approved').stat().st_mode&0o7777,0o600)
        recipe=self.store/'recovery-recipe'
        self.assertEqual(recipe.read_bytes(),b'PDREC001'+self.raw+b'192.168.50.100'.ljust(16,b'\0'))
        self.assertEqual(recipe.stat().st_mode&0o7777,0o600)

    def test_recovery_recipe_precedes_stop(self):
        self.invoke('approved',6)
        self.assertTrue((self.store/'recovery-recipe').exists())
        self.assertNotIn('stop',self.trace.read_text().splitlines())
        self.assertTrue((self.store/'expected-identity').exists())

    def test_not_ready_predecessor_is_not_stopped(self):
        events=self.invoke('not-ready',6)
        self.assertNotIn('stop',events)
        self.assertFalse((self.store/'migration-approved').exists())

    def test_changed_public_identity_cannot_replace_bound_expectation(self):
        self.invoke('approved',6)
        original=(self.store/'expected-identity').read_bytes()
        self.assertNotIn('stop',self.invoke('identity-changed',6))
        self.assertEqual((self.store/'expected-identity').read_bytes(),original)

    def test_changed_address_cannot_rewrite_recovery_recipe(self):
        self.invoke();recipe=self.store/'recovery-recipe';before=recipe.read_bytes()
        events=self.invoke('address-changed',6)
        self.assertNotIn('stop',events);self.assertEqual(recipe.read_bytes(),before)

    def test_foreign_recovery_recipe_refused_before_stop(self):
        recipe=self.store/'recovery-recipe';recipe.write_bytes(b'foreign');recipe.chmod(0o600)
        events=self.invoke(expected=6);self.assertNotIn('stop',events);self.assertEqual(recipe.read_bytes(),b'foreign')

    def test_recovery_recipe_metadata(self):
        self.invoke();recipe=self.store/'recovery-recipe';recipe.chmod(0o644)
        self.assertNotIn('stop',self.invoke(expected=6));recipe.chmod(0o600)
        os.link(recipe,self.root/'alias');self.assertNotIn('stop',self.invoke(expected=6))
        (self.root/'alias').unlink();recipe.unlink();recipe.symlink_to(self.record)
        self.assertNotIn('stop',self.invoke(expected=6))

    def test_partial_recovery_recipe(self):
        recipe=b'PDREC001'+self.raw+b'192.168.50.100'.ljust(16,b'\0')
        partial=self.store/'recovery-recipe.new'
        for length in [0,1,8,100,316,331,332]:
            with self.subTest(length=length):
                partial.write_bytes(recipe[:length]);partial.chmod(0o600)
                self.invoke('approved',6);self.assertFalse(partial.exists())
                self.assertEqual((self.store/'recovery-recipe').read_bytes(),recipe)
                (self.store/'recovery-recipe').unlink()

    def test_foreign_partial_recovery_recipe_preserved(self):
        partial=self.store/'recovery-recipe.new';partial.write_bytes(b'foreign');partial.chmod(0o600)
        self.assertNotIn('stop',self.invoke(expected=6));self.assertEqual(partial.read_bytes(),b'foreign')

    def test_recipe_process_death_before_rename(self):
        events=self.invoke('kill:recipe-file',-9);self.assertNotIn('stop',events)
        self.assertFalse((self.store/'recovery-recipe').exists())
        self.assertTrue((self.store/'recovery-recipe.new').exists())
        self.invoke();self.assertFalse((self.store/'recovery-recipe.new').exists())
        self.assertTrue((self.store/'recovery-recipe').exists())

    def test_recipe_process_death_after_rename(self):
        events=self.invoke('kill:recipe-rename',-9);self.assertNotIn('stop',events)
        self.assertTrue((self.store/'recovery-recipe').exists())
        self.invoke();self.assertFalse((self.store/'recovery-recipe.new').exists())

    def test_final_atomic_rename_is_captured(self):
        self.invoke('rename');self.assertEqual((self.state/'provision.json').read_bytes(),b'after-stop')

    def test_predecessor_mismatch_never_stops(self):
        for fault in ['probe','container','image','executable','image-file','pin']:
            with self.subTest(fault=fault):
                events=self.invoke(fault,6);self.assertNotIn('stop',events);self.assertNotIn('recover',events)
                self.assertFalse((self.store/'migration-approved').exists())

    def test_record_boundary(self):
        for data in [b'',self.raw[:-1],self.raw+b'x',b'PDMIG002'+self.raw[8:],
                     self.raw[:148]+b'0'*64+self.raw[212:],self.raw[:276]+bytes(32),
                     self.raw[:148]+b'--all'+self.raw[153:]]:
            with self.subTest(length=len(data)):
                self.record.write_bytes(data);self.assertEqual(self.invoke(expected=3),[])

    def test_record_metadata(self):
        self.record.chmod(0o644);self.assertEqual(self.invoke(expected=3),[]);self.record.chmod(0o600)
        os.link(self.record,self.root/'alias');self.assertEqual(self.invoke(expected=3),[])
        (self.root/'alias').unlink();self.record.unlink();self.record.symlink_to(self.source/'provision.json')
        self.assertEqual(self.invoke(expected=3),[])

    def test_stop_failure_reconciles_original(self):
        self.assertEqual(self.invoke('stop',6),['probe','pin','stop','recover'])
        self.assertEqual(list(self.state.iterdir()),[])

    def test_post_stop_failure_recovers_original(self):
        self.assertEqual(self.invoke('stopped',6),['probe','pin','stop','recover'])
        self.assertEqual(list(self.state.iterdir()),[])

    def test_failed_recovery_is_distinct(self):
        self.assertEqual(self.invoke('recovery',7),['probe','pin','stop','recover'])

    def test_runtime_exclusion_and_recovery_fence(self):
        self.assertEqual(self.invoke('runtime',6),[])
        self.assertEqual(self.invoke('runtime-after-stop',7),['probe','pin','stop'])

    def test_missing_final_identity_recovers_original(self):
        self.assertEqual(self.invoke('missing',6),['probe','pin','stop','recover'])
        self.assertEqual(list(self.state.iterdir()),[])

    def test_foreign_destination_preserved_and_original_recovers(self):
        (self.state/'foreign').write_bytes(b'keep')
        self.assertEqual(self.invoke(expected=6),['probe','pin','stop','recover'])
        self.assertEqual((self.state/'foreign').read_bytes(),b'keep')

    def test_same_snapshot_retry_and_changed_source_refusal(self):
        self.invoke();self.invoke()
        (self.source/'provision.json').write_bytes(b'changed-after-old-restart')
        events=self.invoke(expected=6);self.assertIn('recover',events)
        self.assertEqual((self.state/'provision.json').read_bytes(),self.config)

    def test_retry_rechecks_source_permissions(self):
        self.invoke();(self.source/'provision.json').chmod(0o666)
        events=self.invoke(expected=6);self.assertIn('recover',events)
        self.assertEqual((self.state/'provision.json').read_bytes(),self.config)

    def test_unexpected_restart_requires_reconciliation(self):
        self.assertEqual(self.invoke('unexpected-restart',6),['probe','pin','stop','stopped','recover'])

    def test_foreign_approval_journal_refused_before_stop(self):
        p=self.store/'migration-approved';p.write_bytes(b'foreign');p.chmod(0o600)
        events=self.invoke(expected=6);self.assertNotIn('stop',events);self.assertEqual(p.read_bytes(),b'foreign')

    def test_partial_approval_journal(self):
        p=self.store/'migration-approved.new';p.write_bytes(self.raw[:32]);p.chmod(0o600)
        self.invoke();self.assertFalse(p.exists())

    def test_foreign_partial_approval_preserved(self):
        p=self.store/'migration-approved.new';p.write_bytes(b'foreign');p.chmod(0o600)
        events=self.invoke(expected=6);self.assertNotIn('stop',events);self.assertEqual(p.read_bytes(),b'foreign')

    def test_lock_and_directory_permissions(self):
        fd=os.open(self.store/'migration.lock',os.O_RDWR|os.O_CREAT,0o600)
        try:
            fcntl.flock(fd,fcntl.LOCK_EX|fcntl.LOCK_NB);self.assertEqual(self.invoke(expected=6),[])
        finally:os.close(fd)
        self.store.chmod(0o777);self.assertEqual(self.invoke(expected=6),[])

    def test_encoder_rejects_wrong_profile_and_noncanonical_context(self):
        for target in [approval.encode(2,'4.2.18.41174','fixture-6300',bytes(range(32))),
                       self.target[:12]+b'\0\0\0\1'+self.target[16:]]:
            with self.assertRaises(ValueError):migration.encode(target,'a'*64,'b'*64,bytes(range(32)))

    def test_fresh_recovery_requires_existing_transaction(self):
        self.assertEqual(self.invoke(expected=6,recover=True),[])
        self.assertFalse((self.store/'migration.lock').exists())
        self.invoke();self.assertEqual(self.invoke(recover=True),['recover-fresh'])

    def test_fresh_recovery_binds_exact_journal(self):
        self.invoke();journal=self.store/'migration-approved'
        for content in [self.raw[:-1],self.raw[:148]+b'c'*64+self.raw[212:]]:
            journal.write_bytes(content);self.assertEqual(self.invoke(expected=6,recover=True),[])

    def test_fresh_recovery_checks_journal_metadata(self):
        self.invoke();journal=self.store/'migration-approved';journal.chmod(0o644)
        self.assertEqual(self.invoke(expected=6,recover=True),[]);journal.chmod(0o600)
        os.link(journal,self.root/'journal-alias');self.assertEqual(self.invoke(expected=6,recover=True),[])
        (self.root/'journal-alias').unlink();journal.unlink();journal.symlink_to(self.record)
        self.assertEqual(self.invoke(expected=6,recover=True),[])

    def test_fresh_recovery_excludes_active_runtime_and_lock(self):
        self.invoke();self.assertEqual(self.invoke('runtime',6,recover=True),[])
        fd=os.open(self.store/'migration.lock',os.O_RDWR)
        try:
            fcntl.flock(fd,fcntl.LOCK_EX|fcntl.LOCK_NB);self.assertEqual(self.invoke(expected=6,recover=True),[])
        finally:os.close(fd)

    def test_fresh_recovery_failure_is_distinct(self):
        self.invoke();self.assertEqual(self.invoke('recovery',7,recover=True),['recover-fresh'])

    def test_running_process_with_wrong_or_unready_public_identity_is_not_recovered(self):
        self.invoke()
        for fault in ['identity-changed','not-ready']:
            self.assertEqual(self.invoke(fault,7,recover=True),['recover-fresh'])

    def test_missing_or_changed_expected_identity_blocks_recovery_before_start(self):
        self.invoke();record=self.store/'expected-identity'
        original=record.read_bytes();record.unlink()
        self.assertEqual(self.invoke(expected=6,recover=True),[])
        changed=bytearray(original);changed[156]=ord('c');record.write_bytes(changed);record.chmod(0o600)
        self.assertEqual(self.invoke(expected=6,recover=True),[])


if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--checker',type=Path,required=True)
    args,rest=parser.parse_known_args();CHECKER=args.checker.resolve()
    unittest.main(argv=['test_migration.py',*rest])
