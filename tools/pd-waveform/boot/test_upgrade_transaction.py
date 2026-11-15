#!/usr/bin/env python3
"""Crash/re-entry tests for upgrade ordering, with inert service adapters."""
import argparse
import fcntl
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
import approval
import migration
import upgrade

CHECKER = None


class UpgradeTransactionTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='pd-upgrade-transaction-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.store = self.root/'store'
        self.store.mkdir(mode=0o700)
        self.capsule = self.root/'aetherpd-upgrade'
        self.capsule.mkdir(mode=0o700)
        self.good = upgrade.encode(1, '4.3.16.42034', 'fixture-8600',
                                   bytes(32), bytes([1])*32, bytes([2])*32, bytes([3])*32)
        self.approved = self.root/'approved'
        self.write(self.approved, self.good)
        old = migration.encode(approval.encode(1, '4.3.16.42034', 'fixture-8600', bytes(32)),
                               'a'*64, 'b'*64, bytes(range(32)))
        self.write(self.store/'migration-approved', old)
        self.write(self.store/'transaction', b'PDTXN001'+old+struct.pack('>I', 5))
        self.write(self.store/'transaction.lock', b'')
        for name, value in [('selected', b'0'), ('running', b'1'), ('identity', b'original')]:
            self.write(self.root/name, value)
        self.installation = {p.name: p.read_bytes() for p in self.store.iterdir()}

    def write(self, path, data):
        path.write_bytes(data)
        path.chmod(0o600)

    def record(self, phase, raw=None):
        return b'PDUTXN01'+(raw or self.good)+struct.pack('>I', phase)

    def call(self, expected=0, fault='', recovery=False, preserve_installation=True):
        p = subprocess.run([str(CHECKER), str(self.root), str(self.approved),
                            'recover' if recovery else 'run', fault], capture_output=True, timeout=5)
        self.assertEqual(p.returncode, expected, (p.stdout, p.stderr))
        if preserve_installation:
            self.assertEqual(self.installation, {n: (self.store/n).read_bytes() for n in self.installation})

    def events(self):
        p = self.root/'events'
        return p.read_text().splitlines() if p.exists() else []

    def phase(self):
        return (self.store/'upgrade-transaction').read_bytes()[-1]

    def test_forward_commit(self):
        self.call()
        self.assertEqual(self.events(), ['quiesce', 'select', 'activate', 'cleanup'])
        self.assertEqual(self.phase(), 4)
        self.assertFalse(self.capsule.exists())
        self.assertEqual((self.root/'selected').read_bytes(), b'1')
        self.assertEqual((self.root/'identity').read_bytes(), b'latest')

    def test_each_failed_forward_callback_rolls_back(self):
        for step in ['quiesce', 'select', 'activate']:
            with self.subTest(step=step):
                self.setUp()
                self.call(10, 'fail:'+step)
                self.assertEqual(self.events()[-3:], ['rollback', 'restore', 'cleanup'])
                self.assertEqual(self.phase(), 7)
                self.assertEqual((self.root/'selected').read_bytes(), b'0')
                self.assertEqual((self.root/'running').read_bytes(), b'1')
                self.assertEqual((self.root/'identity').read_bytes(), b'latest' if step=='activate' else b'original')
                self.assertFalse(self.capsule.exists())

    def test_crashed_callback_reentry_never_advances(self):
        for step in ['quiesce', 'select', 'activate']:
            for recovery in [False, True]:
                with self.subTest(step=step, recovery=recovery):
                    self.setUp()
                    self.call(99, 'crash:'+step)
                    before = self.events()
                    self.call(10, recovery=recovery)
                    self.assertEqual(self.events()[len(before):], ['rollback', 'restore', 'cleanup'])

    def test_recovery_without_journal_never_starts_candidate(self):
        self.call(10, recovery=True)
        self.assertEqual(self.events(), ['rollback', 'restore', 'cleanup'])

    def test_all_forward_publication_boundaries(self):
        for phase in range(1, 5):
            for point in ['upgrade-file', 'upgrade-rename', 'upgrade-durable']:
                with self.subTest(phase=phase, point=point):
                    self.setUp()
                    self.call(99, f'crash:{point}:{phase}')
                    before = self.events()
                    committed = phase==4 and point!='upgrade-file'
                    self.call(0 if committed else 10)
                    self.assertEqual(self.events()[len(before):], ['cleanup'] if committed else ['rollback', 'restore', 'cleanup'])
                    self.assertFalse((self.store/'upgrade-transaction.new').exists())

    def test_all_rollback_publication_boundaries(self):
        for phase in range(5, 8):
            for point in ['upgrade-file', 'upgrade-rename', 'upgrade-durable']:
                with self.subTest(phase=phase, point=point):
                    self.setUp()
                    self.call(99, f'fail:activate,crash:{point}:{phase}')
                    before = self.events()
                    self.call(10, recovery=True)
                    terminal = phase==7 and point!='upgrade-file'
                    self.assertEqual(self.events()[len(before):], ['cleanup'] if terminal else ['rollback', 'restore', 'cleanup'])
                    self.assertEqual((self.root/'identity').read_bytes(), b'latest')

    def test_failed_rollback_or_restore_keeps_authority_for_retry(self):
        for step in ['rollback', 'restore']:
            for mode in ['fail', 'crash']:
                with self.subTest(step=step, mode=mode):
                    self.setUp()
                    self.call(20 if mode=='fail' else 99, f'fail:activate,{mode}:{step}')
                    self.assertTrue(self.capsule.exists())
                    self.call(10, recovery=True)
                    self.assertFalse(self.capsule.exists())
                    self.assertEqual((self.root/'identity').read_bytes(), b'latest')

    def test_terminal_cleanup_failure_cannot_trigger_rollback(self):
        for fault in ['fail:cleanup', 'retain:cleanup', 'crash:cleanup']:
            with self.subTest(fault=fault):
                self.setUp()
                self.call(99 if fault.startswith('crash') else 20, fault)
                self.assertTrue(self.capsule.exists())
                self.assertEqual(self.phase(), 4)
                self.call(recovery=True)
                self.assertNotIn('rollback', self.events())

    def test_completed_reentry_is_idempotent(self):
        for restored in [False, True]:
            with self.subTest(restored=restored):
                self.setUp()
                self.call(10 if restored else 0, 'fail:activate' if restored else '')
                before = self.events()
                self.call(10 if restored else 0, recovery=True)
                self.assertEqual(self.events()[len(before):], ['cleanup'])

    def test_terminal_fsync_failure_keeps_recovery_authority(self):
        self.call(99, 'crash:upgrade-rename:4')
        before = self.events()
        self.call(20, 'close:upgrade-terminal', recovery=True)
        self.assertEqual(before, self.events())
        self.assertTrue(self.capsule.exists())
        self.call(recovery=True)

    def test_active_session_refuses_before_any_effect(self):
        with (self.store/'transaction.lock').open('rb') as lock:
            fcntl.flock(lock, fcntl.LOCK_SH | fcntl.LOCK_NB)
            self.call(30)
        self.assertEqual(self.events(), [])
        self.assertFalse((self.store/'upgrade-transaction').exists())
        self.call()

    def test_missing_or_unsafe_capsule_refuses(self):
        self.capsule.rmdir()
        self.call(30)
        self.capsule.symlink_to('missing')
        self.call(30)
        self.capsule.unlink()
        self.capsule.mkdir(mode=0o755)
        self.call(30)
        self.assertEqual(self.events(), [])

    def test_installation_must_be_committed_and_clean(self):
        original = (self.store/'transaction').read_bytes()
        for phase in [1, 2, 3, 4, 6, 7]:
            self.write(self.store/'transaction', original[:-1]+bytes([phase]))
            self.call(30, preserve_installation=False)
        self.write(self.store/'transaction', original)
        self.write(self.store/'transaction.new', b'')
        self.call(30)
        (self.store/'transaction.new').unlink()
        (self.root/'aetherpd-installation').mkdir(mode=0o700)
        self.call(30)
        self.assertEqual(self.events(), [])

    def test_foreign_or_malformed_journal_preserved(self):
        valid = self.record(1)
        for data in [b'', valid[:-1], valid+b'x', b'foreign!'+valid[8:], valid[:-1]+b'\x00', valid[:-1]+b'\x08']:
            self.write(self.store/'upgrade-transaction', data)
            self.call(30)
            self.assertEqual((self.store/'upgrade-transaction').read_bytes(), data)
        self.assertEqual(self.events(), [])

    def test_partial_initial_record_forces_rollback(self):
        for length in range(365):
            with self.subTest(length=length):
                self.setUp()
                self.write(self.store/'upgrade-transaction.new', self.record(1)[:length])
                self.call(10)
                self.assertEqual(self.events(), ['rollback', 'restore', 'cleanup'])

    def test_foreign_partial_record_preserved(self):
        path = self.store/'upgrade-transaction.new'
        for data in [b'foreign', self.record(1)+b'x', self.record(1)[:20]+b'foreign']:
            self.write(path, data)
            self.call(20)
            self.assertEqual(path.read_bytes(), data)
            self.assertEqual(self.events(), [])

    def test_unsafe_journal_and_lock_metadata_refused_without_changes(self):
        self.write(self.store/'upgrade-transaction', self.record(1))
        for name in ['upgrade-transaction', 'transaction.lock']:
            path = self.store/name
            data = path.read_bytes()
            for mode in [0o644, 0o660, 0o4600]:
                path.chmod(mode)
                self.call(30)
                self.assertEqual(path.read_bytes(), data)
            path.chmod(0o600)
            alias = self.store/'alias'
            os.link(path, alias)
            self.call(30)
            alias.unlink();path.unlink()
            alias.write_bytes(data);alias.chmod(0o600)
            path.symlink_to(alias.name)
            self.call(30)
            path.unlink();alias.unlink()
            os.mkfifo(path, 0o600)
            self.call(30, preserve_installation=False)
            path.unlink();self.write(path, data)
        self.assertEqual(self.events(), [])

    def test_unsafe_partial_preserved_without_effects(self):
        path = self.store/'upgrade-transaction.new'
        self.write(path, self.record(1))
        path.chmod(0o644)
        self.call(20)
        path.chmod(0o600)
        alias = self.store/'alias'
        os.link(path, alias)
        self.call(20)
        alias.unlink();path.unlink()
        path.symlink_to('missing')
        self.call(20)
        self.assertTrue(path.is_symlink())
        path.unlink();os.mkfifo(path, 0o600)
        self.call(20)
        self.assertEqual(self.events(), [])

    def test_next_upgrade_must_follow_actual_selected_release_and_key(self):
        for rolled_back in [False, True]:
            with self.subTest(rolled_back=rolled_back):
                self.setUp()
                self.call(10 if rolled_back else 0, 'fail:activate' if rolled_back else '')
                self.capsule.mkdir(mode=0o700)
                previous = bytes(32) if rolled_back else bytes([1])*32
                previous_key = bytes([2])*32 if rolled_back else bytes([3])*32
                good = upgrade.encode(1, '4.3.16.42034', 'fixture-8600', previous, bytes([4])*32,
                                      previous_key, bytes([5])*32)
                for offset in [24, 56, 100, 288]:
                    bad = bytearray(good);bad[offset] ^= 1
                    self.write(self.approved, bad)
                    self.call(30)
                self.write(self.approved, good)
                self.call()


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--checker', required=True, type=Path)
    args, rest = p.parse_known_args()
    CHECKER = args.checker.resolve()
    unittest.main(argv=[__file__, *rest])
