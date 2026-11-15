#!/usr/bin/env python3
"""Exercise committed-session admission and kernel locks with separate processes."""
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


class SessionGateTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='pd-session-gate-')
        self.addCleanup(self.temp.cleanup)
        self.store = Path(self.temp.name) / 'store'
        self.store.mkdir(mode=0o755)
        self.raw = migration.encode(approval.encode(1, '4.3.16.42034', 'fixture-8600', bytes(range(32))),
                                    'a' * 64, 'b' * 64, bytes(range(32)))
        self.put('migration-approved', self.raw)
        self.put('transaction.lock', b'')
        self.phase(5)

    def put(self, name, data):
        path = self.store / name
        path.write_bytes(data)
        path.chmod(0o600)

    def phase(self, number):
        self.put('transaction', b'PDTXN001' + self.raw + struct.pack('>I', number))

    def invoke(self, expected=3, path=None, runtime=False, activating=False):
        result = subprocess.run([str(CHECKER), str(path or self.store), *(['runtime'] if runtime else [])], input=b'',
                                capture_output=True, timeout=5)
        self.assertEqual(result.returncode, expected, result.stderr.decode(errors='replace'))
        if expected == 0:
            self.assertEqual(result.stdout, b'activating\n' if activating else b'admitted\n')

    def test_only_committed_phase_admits(self):
        for phase in range(9):
            with self.subTest(phase=phase):
                self.phase(phase)
                self.invoke(0 if phase == 5 else 3)

    def test_absent_records_refuse_without_creating_files(self):
        for name in ['transaction', 'migration-approved', 'transaction.lock']:
            path = self.store / name
            data = path.read_bytes()
            path.unlink()
            before = set(self.store.iterdir())
            self.invoke()
            self.assertEqual(before, set(self.store.iterdir()))
            self.put(name, data)

    def test_approval_must_match_completed_transaction(self):
        changed = bytearray(self.raw)
        changed[24] ^= 1
        self.put('migration-approved', changed)
        self.invoke()

    def test_matching_malformed_approval_is_not_authority(self):
        for offset in [0, 8, 19, 56, 84, 148, 212]:
            with self.subTest(offset=offset):
                changed = bytearray(self.raw)
                changed[offset] = 0xff
                self.put('migration-approved', changed)
                self.put('transaction', b'PDTXN001' + changed + struct.pack('>I', 5))
                self.invoke()

    def test_truncated_extended_and_foreign_transaction_refused(self):
        original = (self.store / 'transaction').read_bytes()
        for data in [b'', original[:-1], original + b'x', b'foreign!' + original[8:]]:
            self.put('transaction', data)
            self.invoke()

    def test_partial_publication_requires_installer_recovery(self):
        for data in [b'', b'PDTXN001', (self.store / 'transaction').read_bytes()]:
            self.put('transaction.new', data)
            self.invoke()
            self.assertEqual((self.store / 'transaction.new').read_bytes(), data)
        (self.store / 'transaction.new').unlink()
        self.invoke(0)

    def test_unsafe_file_metadata_refused(self):
        for name in ['transaction', 'migration-approved', 'transaction.lock']:
            path = self.store / name
            data = path.read_bytes()
            for mode in [0o644, 0o660, 0o666, 0o4600]:
                path.chmod(mode)
                self.invoke()
            path.chmod(0o600)
            alias = self.store / 'alias'
            os.link(path, alias)
            self.invoke()
            alias.unlink()
            path.unlink()
            alias.write_bytes(data)
            alias.chmod(0o600)
            path.symlink_to(alias.name)
            self.invoke()
            path.unlink()
            alias.unlink()
            self.put(name, data)
        self.invoke(0)

    def test_nonregular_records_do_not_block(self):
        for name in ['transaction', 'migration-approved', 'transaction.lock']:
            path = self.store / name
            data = path.read_bytes()
            path.unlink()
            os.mkfifo(path, 0o600)
            self.invoke()
            path.unlink()
            path.mkdir(mode=0o700)
            self.invoke()
            path.rmdir()
            self.put(name, data)

    def test_writable_or_symlink_store_refused(self):
        self.store.chmod(0o777)
        self.invoke()
        self.store.chmod(0o755)
        alias = self.store.parent / 'alias'
        alias.symlink_to(self.store.name)
        self.invoke(path=alias)

    def test_active_installer_blocks_session(self):
        with (self.store / 'transaction.lock').open('r+b') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            self.invoke()
        self.invoke(0)

    def test_session_lease_blocks_transaction_until_process_exits(self):
        child = subprocess.Popen([str(CHECKER), str(self.store)], stdin=subprocess.PIPE,
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            self.assertEqual(child.stdout.readline(), b'admitted\n')
            with (self.store / 'transaction.lock').open('r+b') as lock:
                with self.assertRaises(BlockingIOError):
                    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                self.invoke(0)  # Concurrent sessions can hold shared leases.
                child.kill()
                child.communicate(timeout=5)
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        finally:
            if child.poll() is None:
                child.kill()
            child.communicate(timeout=5)

    def test_nonempty_lock_is_not_accepted(self):
        self.put('transaction.lock', b'x')
        self.invoke()

    def test_committed_start_waits_for_installer_authority_removal(self):
        capsule=self.store.parent/'aetherpd-installation';capsule.mkdir(mode=0o700)
        self.invoke();self.invoke(runtime=True)
        capsule.rmdir();capsule.symlink_to('missing')
        self.invoke();self.invoke(runtime=True)
        capsule.unlink();self.invoke(0);self.invoke(0,runtime=True)

    def test_controlled_activation_can_check_readiness_before_capsule_removal(self):
        (self.store.parent/'aetherpd-installation').mkdir(mode=0o700);self.phase(4)
        with (self.store/'transaction.lock').open('r+b') as lock:
            fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
            self.invoke(0,runtime=True,activating=True);self.invoke()

    def test_runtime_requires_commit_without_a_worker(self):
        for phase in range(9):
            self.phase(phase)
            self.invoke(0 if phase == 5 else 3, runtime=True)

    def test_runtime_exception_is_only_activating_with_exclusive_worker(self):
        with (self.store / 'transaction.lock').open('r+b') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            for phase in range(9):
                self.phase(phase)
                self.invoke(0 if phase == 4 else 3, runtime=True, activating=(phase == 4))
                self.invoke()  # Even ACTIVATING never admits a session.

    def test_shared_holder_cannot_stand_in_for_activating_worker(self):
        self.phase(4)
        with (self.store / 'transaction.lock').open('rb') as lock:
            fcntl.flock(lock, fcntl.LOCK_SH | fcntl.LOCK_NB)
            self.invoke(runtime=True)

    def test_runtime_exception_still_validates_approval_and_partial_records(self):
        self.phase(4)
        with (self.store / 'transaction.lock').open('r+b') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            self.put('transaction.new', b'PDTXN001')
            self.invoke(runtime=True)
            (self.store / 'transaction.new').unlink()
            changed = bytearray(self.raw)
            changed[24] ^= 1
            self.put('migration-approved', changed)
            self.invoke(runtime=True)

    def test_worker_death_revokes_runtime_exception(self):
        self.phase(4)
        code = ('import fcntl,sys; f=open(sys.argv[1],"r+b"); '
                'fcntl.flock(f,fcntl.LOCK_EX); print("ready",flush=True); sys.stdin.read(1)')
        child = subprocess.Popen(['python3', '-c', code, str(self.store / 'transaction.lock')],
                                 stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            self.assertEqual(child.stdout.readline(), b'ready\n')
            self.invoke(0, runtime=True, activating=True)
            child.kill()
            child.communicate(timeout=5)
            self.invoke(runtime=True)
        finally:
            if child.poll() is None:
                child.kill()
            child.communicate(timeout=5)

    def upgrade_phase(self, phase):
        raw = upgrade.encode(1, '4.3.16.42034', 'fixture-8600', bytes(32), bytes([1])*32,
                             bytes([2])*32, bytes([3])*32)
        self.put('upgrade-transaction', b'PDUTXN01'+raw+struct.pack('>I', phase))

    def test_upgrade_terminal_requires_authority_removal(self):
        capsule = self.store.parent/'aetherpd-upgrade'
        for phase in range(1, 8):
            self.upgrade_phase(phase)
            self.invoke(0 if phase in (4, 7) else 3)
            self.invoke(0 if phase in (4, 7) else 3, runtime=True)
            capsule.mkdir(mode=0o700)
            self.invoke();self.invoke(runtime=True)
            capsule.rmdir()

    def test_upgrade_runtime_exception_requires_phase_capsule_and_exclusive_worker(self):
        capsule = self.store.parent/'aetherpd-upgrade'
        capsule.mkdir(mode=0o700)
        with (self.store/'transaction.lock').open('r+b') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            for phase in range(1, 8):
                self.upgrade_phase(phase)
                self.invoke(0 if phase in (3, 6) else 3, runtime=True, activating=phase in (3, 6))
                self.invoke()
            capsule.rmdir()
            self.upgrade_phase(3)
            self.invoke(runtime=True)
        capsule.mkdir(mode=0o700)
        with (self.store/'transaction.lock').open('rb') as lock:
            fcntl.flock(lock, fcntl.LOCK_SH | fcntl.LOCK_NB)
            self.invoke(runtime=True)

    def test_upgrade_partial_and_unsafe_records_refuse(self):
        self.upgrade_phase(4)
        original = (self.store/'upgrade-transaction').read_bytes()
        for data in [b'', original[:-1], original+b'x', b'foreign!'+original[8:]]:
            self.put('upgrade-transaction', data)
            self.invoke();self.invoke(runtime=True)
        self.put('upgrade-transaction', original)
        for mode in [0o644, 0o660, 0o4600]:
            (self.store/'upgrade-transaction').chmod(mode)
            self.invoke();self.invoke(runtime=True)
        (self.store/'upgrade-transaction').chmod(0o600)
        alias = self.store/'alias'
        os.link(self.store/'upgrade-transaction', alias)
        self.invoke();self.invoke(runtime=True)
        alias.unlink()
        self.put('upgrade-transaction.new', b'')
        self.invoke();self.invoke(runtime=True)

    def test_upgrade_worker_death_revokes_activation_and_restore(self):
        (self.store.parent/'aetherpd-upgrade').mkdir(mode=0o700)
        code = ('import fcntl,sys; f=open(sys.argv[1],"r+b"); '
                'fcntl.flock(f,fcntl.LOCK_EX); print("ready",flush=True); sys.stdin.read(1)')
        for phase in [3, 6]:
            self.upgrade_phase(phase)
            child = subprocess.Popen(['python3', '-c', code, str(self.store/'transaction.lock')],
                                     stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            try:
                self.assertEqual(child.stdout.readline(), b'ready\n')
                self.invoke(0, runtime=True, activating=True)
                child.kill();child.communicate(timeout=5)
                self.invoke(runtime=True);self.invoke()
            finally:
                if child.poll() is None:
                    child.kill()
                child.communicate(timeout=5)

    def test_upgrade_capsule_unsafe_or_partial_cannot_authorize_runtime(self):
        capsule = self.store.parent/'aetherpd-upgrade'
        self.upgrade_phase(3)
        with (self.store/'transaction.lock').open('r+b') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            capsule.symlink_to('missing')
            self.invoke(runtime=True)
            capsule.unlink();capsule.mkdir(mode=0o755)
            self.invoke(runtime=True)
            capsule.chmod(0o700)
            self.invoke(0, runtime=True, activating=True)
            self.put('upgrade-transaction.new', b'')
            self.invoke(runtime=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--checker', required=True, type=Path)
    args, remaining = parser.parse_known_args()
    CHECKER = args.checker.resolve()
    unittest.main(argv=[__file__, *remaining])
