#!/usr/bin/env python3
"""Restart the journal driver after process death using a separate resource model."""
import argparse
import fcntl
import os
from pathlib import Path
import signal
import struct
import subprocess
import tempfile
import unittest
import approval
import migration
import first_install

CHECKER=None


class TransactionTest(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(prefix='pd-transaction-');self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name);self.store=self.root/'store';self.store.mkdir(mode=0o755)
        self.raw=migration.encode(approval.encode(1,'4.3.16.42034','fixture-8600',bytes(range(32))),
                                  'a'*64,'b'*64,bytes(range(32)))
        self.record=self.root/'approval';self.record.write_bytes(self.raw);self.record.chmod(0o600)
        self.model=self.root/'model';self.reset()

    def reset(self):
        for p in self.store.iterdir():p.unlink()
        self.model.write_bytes(struct.pack('=12I',1,1,0,41,0,1,0,0,0,0,0,0))

    def state(self):
        return dict(zip(['old_present','old_running','new_running','old_identity','new_identity','authority',
                         'prepared','copied','retired','activated','rollbacks','cleanups'],struct.unpack('=12I',self.model.read_bytes())))

    def invoke(self,fault='',expected=0,recover=False,authorize=False):
        result=subprocess.run([str(CHECKER),str(self.store),str(self.record),str(self.model),fault,*(['authorize-retry'] if authorize else ['recover'] if recover else [])],capture_output=True,timeout=5)
        self.assertEqual(result.returncode,expected,result.stderr.decode(errors='replace'))

    def assert_owner(self,new):
        s=self.state();self.assertEqual(s['new_running'],int(new));self.assertEqual(s['old_running'],int(not new))
        self.assertEqual(s['authority'],0)
        self.assertEqual(s['new_identity'] if new else s['old_identity'],42 if s['activated'] else 41)

    def fresh_failed(self):
        self.raw=first_install.encode(approval.encode(1,'4.3.16.42034','fixture-8600',bytes(range(32))),b't'*32,'fixture',b'n'*32)
        self.record.write_bytes(self.raw)
        self.invoke('fail:activate',expected=10)
        values=list(struct.unpack('=12I',self.model.read_bytes()));values[5]=1
        self.model.write_bytes(struct.pack('=12I',*values))

    def test_retry_authorization_requires_exact_failed_fresh_transaction(self):
        self.invoke(authorize=True,expected=12)
        self.fresh_failed();failed=(self.store/'transaction').read_bytes()
        self.invoke(authorize=True)
        self.assertEqual((self.store/'transaction-failed').read_bytes(),failed)
        self.assertEqual((self.store/'transaction-retry').read_bytes(),b'PDRETRY1'+self.raw)
        self.invoke();self.assertFalse((self.store/'transaction-retry').exists())
        self.assertEqual((self.store/'transaction-failed').read_bytes(),failed)
        self.invoke(authorize=True,expected=12)

    def test_boot_discards_retry_and_preserves_failed_state(self):
        self.fresh_failed();self.invoke(authorize=True);before=self.state()['activated']
        self.invoke(recover=True,expected=10)
        self.assertEqual(self.state()['activated'],before);self.assertFalse((self.store/'transaction-retry').exists())
        self.assertEqual((self.store/'transaction').read_bytes(),b'PDTXN001'+self.raw+struct.pack('>I',7))

    def test_interrupted_retry_publication_never_authorizes_forward(self):
        self.fresh_failed();self.invoke('kill:transaction-retry:0',expected=-signal.SIGKILL,authorize=True)
        self.assertTrue((self.store/'transaction-retry.new').exists());before=self.state()['activated']
        self.invoke(expected=10);self.assertEqual(self.state()['activated'],before)
        self.assertFalse((self.store/'transaction-retry.new').exists())

    def test_retry_authorization_is_serialized_and_preserves_foreign_partial(self):
        self.fresh_failed();fd=os.open(self.store/'transaction.lock',os.O_RDWR)
        try:
            fcntl.flock(fd,fcntl.LOCK_EX|fcntl.LOCK_NB);self.invoke(authorize=True,expected=12)
            self.assertFalse((self.store/'transaction-failed').exists())
        finally:os.close(fd)
        partial=self.store/'transaction-failed.new';partial.write_bytes(b'foreign');partial.chmod(0o600)
        self.invoke(authorize=True,expected=12);self.assertEqual(partial.read_bytes(),b'foreign')
        self.assertFalse((self.store/'transaction-retry').exists())

    def test_retry_consumption_is_one_shot_even_after_process_death(self):
        self.fresh_failed();self.invoke(authorize=True);before=self.state()['activated']
        self.invoke('kill:retry-consumed:0',expected=-signal.SIGKILL)
        self.invoke(expected=10);self.assertEqual(self.state()['activated'],before)

    def test_commit_releases_authority_and_preserves_new_identity(self):
        self.invoke();self.assert_owner(True);s=self.state()
        self.assertEqual([s[n] for n in ['prepared','copied','retired','activated','rollbacks']], [1,1,1,1,0])
        self.assertEqual((self.store/'transaction').read_bytes(),b'PDTXN001'+self.raw+struct.pack('>I',5))

    def test_boot_recovery_before_first_journal_never_installs(self):
        self.invoke(expected=10,recover=True);self.assert_owner(False)
        self.assertEqual([self.state()[n] for n in ['prepared','copied','retired','activated','rollbacks']], [0,0,0,0,1])
        self.assertEqual((self.store/'transaction').read_bytes(),b'PDTXN001'+self.raw+struct.pack('>I',7))

    def test_boot_recovery_after_partial_initial_write_never_installs(self):
        self.invoke('kill:file:1',-signal.SIGKILL)
        self.assertFalse((self.store/'transaction').exists())
        self.invoke(expected=10,recover=True);self.assert_owner(False)
        self.assertEqual(self.state()['prepared'],0)

    def test_boot_recovery_at_each_forward_boundary_only_rolls_back(self):
        for action in ['prepare','copy','retire','activate']:
            with self.subTest(action=action):
                self.reset();self.invoke('kill:'+action,-signal.SIGKILL);before=self.state()
                self.invoke(expected=10,recover=True);after=self.state();self.assert_owner(False)
                for name in ['prepared','copied','retired','activated']:self.assertEqual(before[name],after[name])

    def test_boot_recovery_of_terminal_records_only_finishes_cleanup(self):
        self.invoke('fail:cleanup',11);before=self.state();self.invoke(recover=True)
        self.assertEqual(self.state()['rollbacks'],before['rollbacks']);self.assert_owner(True)
        self.reset();self.invoke(expected=10,recover=True);before=self.state()
        self.invoke(expected=10,recover=True)
        self.assertEqual(self.state()['rollbacks'],before['rollbacks']);self.assert_owner(False)

    def test_boot_recovery_preserves_foreign_partial_without_actions(self):
        p=self.store/'transaction.new';p.write_bytes(b'foreign');p.chmod(0o600)
        before=self.model.read_bytes();self.invoke(expected=11,recover=True)
        self.assertEqual(self.model.read_bytes(),before);self.assertEqual(p.read_bytes(),b'foreign')

    def test_terminal_retry_only_cleans_up(self):
        self.invoke();first=self.state();self.invoke();second=self.state()
        self.assertEqual({k:v for k,v in first.items() if k!='cleanups'}, {k:v for k,v in second.items() if k!='cleanups'})
        self.assert_owner(True)

    def test_action_failure_rolls_back_latest_identity(self):
        for action in ['prepare','copy','retire','activate']:
            with self.subTest(action=action):
                self.reset();self.invoke('fail:'+action,10);self.assert_owner(False)
                self.assertEqual(self.state()['rollbacks'],1)

    def test_process_death_after_each_action(self):
        for action in ['prepare','copy','retire','activate','cleanup']:
            with self.subTest(action=action):
                self.reset();self.invoke('kill:'+action,-signal.SIGKILL)
                committed=action=='cleanup';self.invoke(expected=0 if committed else 10);self.assert_owner(committed)

    def test_process_death_during_each_forward_journal_write(self):
        for phase in range(1,6):
            for boundary in ['file','rename','durable']:
                with self.subTest(phase=phase,boundary=boundary):
                    self.reset();self.invoke(f'kill:{boundary}:{phase}',-signal.SIGKILL)
                    committed=(phase==5 and boundary!='file') or (phase==1 and boundary=='file')
                    self.invoke(expected=0 if committed else 10);self.assert_owner(committed)
                    self.assertFalse((self.store/'transaction.new').exists())

    def test_write_failure_does_not_run_unjournaled_action(self):
        for phase in range(1,6):
            self.reset();self.invoke(f'fail:file:{phase}',11)
            self.assertEqual(sum(self.state()[n] for n in ['prepared','copied','retired','activated']),phase-1)
            self.invoke(expected=0 if phase==1 else 10);self.assert_owner(phase==1)

    def test_rollback_death_is_reconciled_again(self):
        for boundary in ['file:6','rename:6','durable:6','rollback','file:7','rename:7','durable:7']:
            with self.subTest(boundary=boundary):
                self.reset();self.invoke('kill:activate',-signal.SIGKILL)
                self.invoke('kill:'+boundary,-signal.SIGKILL)
                self.invoke(expected=10);self.assert_owner(False)

    def test_failed_rollback_retains_authority_until_retry(self):
        self.invoke('kill:activate',-signal.SIGKILL);self.invoke('fail:rollback',11)
        self.assertEqual(self.state()['authority'],1);self.invoke(expected=10);self.assert_owner(False)

    def test_cleanup_failure_never_rolls_back_committed_installation(self):
        self.invoke('fail:cleanup',11);self.invoke();self.assert_owner(True)
        self.assertEqual(self.state()['rollbacks'],0)

    def test_retry_retains_authority_when_terminal_durability_cannot_be_established(self):
        self.invoke('kill:rename:5',-signal.SIGKILL)
        self.invoke('close:terminal:5',11)
        self.assertEqual(self.state()['authority'],1);self.assertEqual(self.state()['new_running'],1)
        self.invoke();self.assert_owner(True)

    def test_foreign_journal_does_not_touch_resources(self):
        original=self.model.read_bytes();p=self.store/'transaction'
        for data in [b'foreign',b'PDTXN001'+self.raw[:-1]+b'X'+struct.pack('>I',1),
                     b'PDTXN001'+self.raw+struct.pack('>I',8)]:
            p.write_bytes(data);p.chmod(0o600);self.invoke(expected=12);self.assertEqual(self.model.read_bytes(),original)

    def test_foreign_partial_is_preserved(self):
        p=self.store/'transaction.new';p.write_bytes(b'foreign');p.chmod(0o600)
        original=self.model.read_bytes();self.invoke(expected=11)
        self.assertEqual(p.read_bytes(),b'foreign');self.assertEqual(self.model.read_bytes(),original)

    def test_interrupted_partial_commit_does_not_override_published_phase(self):
        pending=b'PDTXN001'+self.raw+struct.pack('>I',5)
        for size in [0,7,8,147,148,315,316,319,320]:
            with self.subTest(size=size):
                self.reset();self.invoke('kill:activate',-signal.SIGKILL)
                p=self.store/'transaction.new';p.write_bytes(pending[:size]);p.chmod(0o600)
                self.invoke(expected=10);self.assert_owner(False);self.assertFalse(p.exists())

    def test_invalid_partial_phase_and_metadata_are_preserved(self):
        self.invoke('kill:activate',-signal.SIGKILL);p=self.store/'transaction.new'
        data=b'PDTXN001'+self.raw+struct.pack('>I',8);p.write_bytes(data);p.chmod(0o600)
        original=self.model.read_bytes();self.invoke(expected=11)
        self.assertEqual(p.read_bytes(),data);self.assertEqual(self.model.read_bytes(),original)
        p.write_bytes(b'PDTXN001'+self.raw);p.chmod(0o666);self.invoke(expected=11)
        self.assertEqual(self.model.read_bytes(),original)

    def test_journal_and_lock_metadata(self):
        self.invoke('kill:prepare',-signal.SIGKILL);p=self.store/'transaction';original=self.model.read_bytes()
        p.chmod(0o644);self.invoke(expected=12);p.chmod(0o600)
        os.link(p,self.root/'alias');self.invoke(expected=12);(self.root/'alias').unlink()
        data=p.read_bytes();p.unlink();(self.root/'journal').write_bytes(data);p.symlink_to(self.root/'journal')
        self.invoke(expected=12);self.assertEqual(self.model.read_bytes(),original)

    def test_lock_and_store_permissions(self):
        fd=os.open(self.store/'transaction.lock',os.O_RDWR|os.O_CREAT,0o600)
        try:
            fcntl.flock(fd,fcntl.LOCK_EX|fcntl.LOCK_NB);self.invoke(expected=12)
        finally:os.close(fd)
        self.store.chmod(0o777);self.invoke(expected=12)


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--checker',type=Path,required=True)
    a,rest=p.parse_known_args();CHECKER=a.checker.resolve();unittest.main(argv=['test_transaction.py',*rest])
