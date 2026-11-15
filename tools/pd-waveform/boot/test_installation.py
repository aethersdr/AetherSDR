#!/usr/bin/env python3
"""Terminal journal/selector binding, atomic mirror publication and crash recovery."""
import argparse
import fcntl
import hashlib
import os
from pathlib import Path
import signal
import struct
import subprocess
import tempfile
import unittest
import approval
import migration
import upgrade

CHECKER = None
DISK_DIRECTORY = None
GO_SHIM = None
EXPORT_FIXTURES = None

class InstallationTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='pd-installation-', dir='/dev/shm')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.store = self.root / 'store'
        self.store.mkdir(mode=0o700)
        self.runtime = self.root / 'run/aetherpd'
        self.runtime.mkdir(mode=0o755, parents=True)
        self.plan = upgrade.encode(1, '4.3.16.42034', 'fixture-8600', bytes([1])*32,
                                   bytes([9])*32, bytes([2])*32, bytes([3])*32)
        self.digest = hashlib.blake2b(self.plan, digest_size=32).digest()
        initial = migration.encode(approval.encode(1, '4.3.16.42034', 'fixture-8600', bytes([1])*32),
                                   'a'*64, 'b'*64, bytes([4])*32)
        self.write(self.store/'migration-approved', initial)
        self.write(self.store/'transaction', b'PDTXN001'+initial+struct.pack('>I', 5))
        self.write(self.store/'transaction.lock', b'')
        generation = self.store/'upgrade-generations'/self.digest.hex()
        (generation/'pair').mkdir(mode=0o700, parents=True)
        (self.store/'upgrade-generations').chmod(0o700)
        generation.chmod(0o700)
        self.control = generation/'control'
        self.control.mkdir(mode=0o700)
        self.write(generation/'pair/approved', self.plan)
        self.write(self.store/'upgrade-generation', b'PDGEN001'+self.digest)
        self.phase(4)

    @staticmethod
    def write(path, data, mode=0o600):
        path.write_bytes(data)
        path.chmod(mode)

    def phase(self, phase):
        self.write(self.store/'upgrade-transaction', b'PDUTXN01'+self.plan+struct.pack('>I', phase))
        choice = phase == 4
        self.selected_digest = bytes([9 if choice else 1])*32
        self.selected_key = bytes([3 if choice else 2])*32
        self.write(self.control/'active', b'PDPAIR01'+bytes([int(choice)])+bytes(7)+self.digest+self.selected_digest)

    def expected(self, phase=4):
        return (b'PDVIEW01'+struct.pack('>II', phase, 1)+self.digest+self.selected_digest+self.selected_key
                +b'4.3.16.42034'.ljust(28, b'\0')+b'fixture-8600'.ljust(64, b'\0'))

    def invoke(self, expected=0, kill=None, runtime=None):
        result = subprocess.run([str(CHECKER), str(self.store), str(runtime or self.runtime)]
                                +([] if kill is None else [str(kill)]), capture_output=True, timeout=5)
        self.assertEqual(result.returncode, expected, result.stderr.decode(errors='replace'))

    def snapshot(self):
        return {p.name:(p.lstat().st_ino, p.lstat().st_mode,
                        os.readlink(p) if p.is_symlink() else p.read_bytes()) for p in self.runtime.iterdir()}

    def refused(self):
        before = self.snapshot()
        self.invoke(1)
        self.assertEqual(self.snapshot(), before)

    def test_committed_and_rollback_exact_record(self):
        for phase in [4, 7]:
            self.phase(phase)
            self.invoke()
            p = self.runtime/'installation'
            self.assertEqual(p.read_bytes(), self.expected(phase))
            self.assertEqual(p.stat().st_mode & 0o7777, 0o444)
            self.assertFalse((self.runtime/'.installation.new').exists())

    def test_attempt_nonce_reaches_public_mirror(self):
        self.invoke()
        previous = (self.runtime/'installation').read_bytes()
        old_digest = self.digest
        self.plan = upgrade.encode(1, '4.3.16.42034', 'fixture-8600', bytes([1])*32,
                                   bytes([9])*32, bytes([2])*32, bytes([3])*32, attempt=bytes([5])*32)
        self.digest = hashlib.blake2b(self.plan, digest_size=32).digest()
        parent = self.store/'upgrade-generations'
        (parent/old_digest.hex()).rename(parent/self.digest.hex())
        self.control = parent/self.digest.hex()/'control'
        self.write(parent/self.digest.hex()/'pair/approved', self.plan)
        self.write(self.store/'upgrade-generation', b'PDGEN001'+self.digest)
        for phase in [7, 4]:
            self.phase(phase)
            self.invoke()
            actual = (self.runtime/'installation').read_bytes()
            self.assertEqual(actual, self.expected(phase))
            self.assertNotEqual(actual[16:48], previous[16:48])
            if EXPORT_FIXTURES is not None:
                EXPORT_FIXTURES.mkdir(parents=True, exist_ok=True)
                (EXPORT_FIXTURES/'approved-attempt.hex').write_text(self.plan.hex()+'\n')
                (EXPORT_FIXTURES/('attempt-'+str(phase)+'.hex')).write_text(actual.hex()+'\n')

    def test_native_publication_read_by_go(self):
        if GO_SHIM is None:
            self.skipTest('--go-shim required for native-to-Go interoperability test')
        for phase in [4, 7]:
            self.phase(phase)
            self.invoke()
            env = dict(os.environ, AETHERPD_INSTALLATION_TEST_ROOT=str(self.root),
                       AETHERPD_INSTALLATION_TEST_PLAN=self.digest.hex(),
                       AETHERPD_INSTALLATION_TEST_PHASE=str(phase))
            if EXPORT_FIXTURES is not None:
                EXPORT_FIXTURES.mkdir(parents=True, exist_ok=True)
                (EXPORT_FIXTURES/'approved.hex').write_text(self.plan.hex()+'\n')
                env['AETHERPD_INSTALLATION_EXPORT'] = str(EXPORT_FIXTURES)
            result = subprocess.run(['go', 'test', '-race', '-count=1', '-run',
                                     '^TestPDInstallationNativePublisher$', '.'], cwd=GO_SHIM,
                                    env=env, capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)

    def test_all_nonterminal_phases_refuse(self):
        for phase in [1, 2, 3, 5, 6]:
            self.phase(phase)
            self.refused()

    def test_temporary_authority_and_pending_records_refuse(self):
        for name in ['aetherpd-upgrade', 'aetherpd-installation']:
            p = self.root/name
            p.mkdir(mode=0o700)
            self.refused()
            p.rmdir()
        for name in ['transaction.new', 'upgrade-transaction.new', 'upgrade-generation.new']:
            p = self.store/name
            self.write(p, b'')
            self.refused()
            p.unlink()

    def test_active_transaction_or_publisher_refuses(self):
        for path in [self.store/'transaction.lock', self.runtime]:
            fd = os.open(path, os.O_RDONLY)
            try:
                fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                self.refused()
            finally:
                os.close(fd)

    def test_selector_choice_plan_release_and_reserved_bytes_bound(self):
        p = self.control/'active'
        original = p.read_bytes()
        for offset in [8, 9, 16, 48, 79]:
            changed = bytearray(original)
            changed[offset] ^= 1
            self.write(p, changed)
            self.refused()
        self.write(p, original)
        self.invoke()

    def test_uncommitted_original_installation_refuses(self):
        p = self.store/'transaction'
        original = p.read_bytes()
        for phase in [1, 2, 3, 4, 6, 7]:
            self.write(p, original[:-4]+struct.pack('>I', phase))
            self.refused()

    def test_crash_each_publication_step_keeps_old_or_new_snapshot(self):
        for point in range(1, 5):
            self.phase(4)
            self.invoke()
            old = self.expected()
            with (self.runtime/'installation').open('rb') as pinned:
                self.phase(7)
                new = self.expected(7)
                self.invoke(-signal.SIGKILL, point)
                self.assertIn((self.runtime/'installation').read_bytes(), [old, new])
                self.invoke()
                self.assertEqual((self.runtime/'installation').read_bytes(), new)
                self.assertEqual(pinned.read(), old)

    def test_foreign_partial_preserved(self):
        for data, mode in [(b'foreign', 0o600), (b'', 0o644), (bytes(205), 0o600)]:
            self.write(self.runtime/'.installation.new', data, mode)
            self.refused()

    def test_links_and_invalid_mirror_metadata_preserved(self):
        self.invoke()
        p = self.runtime/'installation'
        for mode in [0o600, 0o644, 0o4444]:
            p.chmod(mode)
            self.refused()
        p.chmod(0o444)
        alias = self.root/'alias'
        os.link(p, alias)
        self.refused()
        alias.unlink()
        p.rename(alias)
        p.symlink_to(alias)
        self.refused()

    def test_writable_runtime_refuses(self):
        self.runtime.chmod(0o777)
        self.refused()
        self.runtime.chmod(0o755)

    def test_non_tmpfs_refuses(self):
        if DISK_DIRECTORY is None:
            self.skipTest('--disk-directory required for non-tmpfs negative test')
        with tempfile.TemporaryDirectory(dir=DISK_DIRECTORY) as disk:
            self.invoke(1, runtime=Path(disk))

if __name__ == '__main__':
    p = argparse.ArgumentParser()
    p.add_argument('--checker', type=Path, required=True)
    p.add_argument('--disk-directory', type=Path)
    p.add_argument('--go-shim', type=Path)
    p.add_argument('--export-fixtures', type=Path)
    args, rest = p.parse_known_args()
    EXPORT_FIXTURES = args.export_fixtures.resolve() if args.export_fixtures else None
    GO_SHIM = args.go_shim.resolve() if args.go_shim else None
    DISK_DIRECTORY = args.disk_directory
    CHECKER = args.checker.resolve()
    unittest.main(argv=[__file__, *rest])
