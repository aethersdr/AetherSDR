#!/usr/bin/env python3
"""Durable installer inputs, restart admission and terminal authority removal."""
import argparse
import fcntl
import os
from pathlib import Path
import signal
import struct
import subprocess
import unittest
import approval
import bundle
import migration
import release
import test_release

CHECKER = None


class CapsuleTest(unittest.TestCase):
    def setUp(self):
        test_release.ReleaseTest.setUp(self)
        self.files['BOOT-LICENSES.txt'] = b'fixture licenses\n' * 2000
        self.good, _ = release.assemble(self.files, 'flex8600', '4.3.16.42034', bytes(32), self.seed)
        self.parent = self.root / 'parent'
        self.parent.mkdir(mode=0o700)
        self.store = self.root / 'store'
        self.store.mkdir(mode=0o700)
        self.worker = self.root / 'worker'
        self.worker.write_bytes(bytes(self.raw) * 16)
        self.worker.chmod(0o700)
        self.input = self.root / 'release'
        self.input.write_bytes(self.good)
        self.input.chmod(0o600)
        self.plan = self.root / 'plan'
        self.raw_plan = migration.encode(approval.encode(1, '4.3.16.42034', 'fixture-8600', bundle.blake(self.good)),
                                         'a' * 64, 'b' * 64, bytes(range(32)))
        self.plan.write_bytes(self.raw_plan)
        self.plan.chmod(0o600)
        self.phase(5)
        self.capsule = self.parent / 'aetherpd-installation'
        self.pending = self.parent / '.aetherpd-installation'
        self.retired = self.parent / '.aetherpd-installation-retired'
        self.units = self.root / 'units'
        self.units.mkdir(mode=0o755)

    def phase(self, phase, plan=None):
        path = self.store / 'transaction'
        path.write_bytes(b'PDTXN001' + (self.raw_plan if plan is None else plan) + struct.pack('>I', phase))
        path.chmod(0o600)

    def args(self, command, fault='', worker=None):
        return [str(CHECKER), command, str(self.key), str(self.parent), str(worker or self.worker),
                str(self.input), str(self.plan), str(self.store), fault]

    def invoke(self, command='publish', fault='', expected=0, worker=None):
        result = subprocess.run(self.args(command, fault, worker), input=b'', capture_output=True, timeout=10)
        self.assertEqual(result.returncode, expected, result.stderr.decode(errors='replace'))

    def reset(self):
        import shutil
        for path in self.parent.iterdir():
            if path.is_dir() and not path.is_symlink():
                shutil.rmtree(path)
            else:
                path.unlink()

    def verify(self):
        self.assertEqual(sorted(p.name for p in self.capsule.iterdir()), ['intent', 'release', 'worker'])
        self.assertEqual(self.capsule.stat().st_mode & 0o7777, 0o700)
        self.assertEqual((self.capsule / 'worker').read_bytes(), self.worker.read_bytes())
        self.assertEqual((self.capsule / 'release').read_bytes(), self.good)
        intent = (self.capsule / 'intent').read_bytes()
        self.assertEqual(intent, b'PDCAP001' + self.key.read_bytes() + bundle.blake(self.worker.read_bytes()) + self.raw_plan)
        for name, mode in [('intent', 0o600), ('release', 0o600), ('worker', 0o700)]:
            self.assertEqual((self.capsule / name).stat().st_mode & 0o7777, mode)
        self.invoke('open')

    def bind(self):
        path=self.store/'migration-approved';path.write_bytes(self.raw_plan);path.chmod(0o600)

    def test_normal_boot_reconciles_inert_terminal_definitions(self):
        self.bind();self.invoke('arm',worker=self.units)
        (self.units/'multi-user.target.wants/aetherpd-recover.service').unlink()
        self.invoke('reconcile',worker=self.units)
        self.assertFalse((self.units/'aetherpd-recover.service').exists())
        self.assertFalse((self.units/'aetherpd-install.service').exists())

    def test_normal_boot_does_not_dispatch_or_detach_active_capsule(self):
        self.bind();self.invoke();self.invoke('arm',worker=self.units)
        self.invoke('reconcile',expected=1,worker=self.units);self.verify()
        self.phase(4);self.invoke('reconcile',worker=self.units);self.verify()
        self.assertTrue((self.units/'aetherpd-recover.service').exists())

    def test_normal_boot_refuses_corrupt_binding_without_unit_deletion(self):
        self.bind();self.invoke('arm',worker=self.units);(self.store/'migration-approved').write_bytes(b'foreign')
        self.invoke('reconcile',expected=1,worker=self.units)
        self.assertTrue((self.units/'aetherpd-recover.service').exists())

    def test_normal_boot_without_binding_leaves_foreign_files(self):
        self.invoke('arm',worker=self.units);self.invoke('reconcile',worker=self.units)
        self.assertTrue((self.units/'aetherpd-recover.service').exists())

    def test_publish_reauthenticate_and_idempotency(self):
        self.invoke()
        self.verify()
        inode = (self.capsule / 'worker').stat().st_ino
        self.invoke()
        self.verify()
        self.assertEqual((self.capsule / 'worker').stat().st_ino, inode)

    def test_fresh_process_needs_no_original_input_descriptors(self):
        self.invoke()
        for path in [self.worker, self.input, self.plan]:
            path.unlink()
        self.invoke('open')
        self.invoke('retire')
        self.assertEqual([p for p in self.parent.iterdir() if p.name != '.aetherpd-installation.lock'], [])

    def test_tampered_release_refused_before_publication(self):
        bad = bytearray(self.good)
        bad[-1] ^= 1
        self.input.write_bytes(bad)
        self.invoke(expected=1)
        self.assertEqual(list(self.parent.iterdir()), [])

    def test_operator_release_digest_must_match(self):
        changed = bytearray(self.raw_plan)
        changed[24] ^= 1
        self.plan.write_bytes(changed)
        self.invoke(expected=1)
        self.assertEqual(list(self.parent.iterdir()), [])

    def test_existing_capsule_is_bound_to_exact_migration(self):
        self.invoke()
        before = (self.capsule / 'intent').read_bytes()
        changed = bytearray(self.raw_plan)
        changed[148] = ord('c')
        self.plan.write_bytes(changed)
        self.invoke(expected=1)
        self.assertEqual((self.capsule / 'intent').read_bytes(), before)

    def test_worker_source_requires_protected_regular_single_link_file(self):
        self.worker.chmod(0o777)
        self.invoke(expected=1)
        self.worker.chmod(0o700)
        alias = self.root / 'alias'
        os.link(self.worker, alias)
        self.invoke(expected=1)
        alias.unlink()
        alias.symlink_to(self.worker)
        self.invoke(expected=1, worker=alias)
        self.assertEqual(list(self.parent.iterdir()), [])

    def test_process_death_at_publication_boundaries(self):
        points = ['intent-bytes', 'intent-file', 'intent-rename', 'intent-durable',
                  'worker-bytes', 'worker-file', 'worker-rename', 'release-bytes',
                  'release-file', 'release-rename', 'capsule-durable', 'capsule-rename']
        for point in points:
            with self.subTest(point=point):
                self.reset()
                self.invoke(fault='kill:' + point, expected=-signal.SIGKILL)
                self.invoke()
                self.verify()

    def test_failed_publication_can_resume(self):
        for point in ['worker-bytes', 'release-file', 'capsule-rename']:
            self.reset()
            self.invoke(fault='fail:' + point, expected=1)
            self.invoke()
            self.verify()

    def test_foreign_partial_prefix_is_preserved(self):
        self.invoke(fault='kill:worker-bytes', expected=-signal.SIGKILL)
        partial = self.pending / 'worker.new'
        original = partial.read_bytes()
        partial.write_bytes(b'x' + original[1:])
        self.invoke(expected=1)
        self.assertEqual(partial.read_bytes(), b'x' + original[1:])
        self.assertFalse(self.capsule.exists())

    def test_published_file_metadata_and_unknown_entries_refused(self):
        self.invoke()
        for name in ['intent', 'worker', 'release']:
            path = self.capsule / name
            mode = path.stat().st_mode & 0o7777
            path.chmod(0o777)
            self.invoke('open', expected=1)
            path.chmod(mode)
            alias = self.root / 'alias'
            os.link(path, alias)
            self.invoke('open', expected=1)
            alias.unlink()
        foreign = self.capsule / 'foreign'
        foreign.write_bytes(b'keep')
        self.invoke('open', expected=1)
        self.assertEqual(foreign.read_bytes(), b'keep')

    def test_reauthentication_detects_every_changed_member(self):
        self.invoke()
        for name in ['intent', 'worker', 'release']:
            path = self.capsule / name
            original = path.read_bytes()
            path.write_bytes(b'x' + original[1:])
            self.invoke('open', expected=1)
            path.write_bytes(original)
        self.invoke('open')

    def test_parent_and_capsule_symlinks_refused(self):
        actual = self.parent
        alias = self.root / 'parent-alias'
        alias.symlink_to(actual)
        self.parent = alias
        self.invoke(expected=1)
        self.parent = actual
        self.parent.chmod(0o777)
        self.invoke(expected=1)
        self.parent.chmod(0o700)
        self.capsule.symlink_to(self.store)
        self.invoke(expected=1)
        self.assertFalse(self.pending.exists())

    def test_lease_serializes_publication_and_fresh_workers(self):
        self.invoke()
        child = subprocess.Popen(self.args('hold'), stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            self.assertEqual(child.stdout.readline(), b'authenticated\n')
            self.invoke(expected=1)
            self.invoke('open', expected=1)
            self.invoke('retire', expected=1)
            child.kill()
            child.communicate(timeout=5)
            self.invoke('open')
        finally:
            if child.poll() is None:
                child.kill()
            child.communicate(timeout=5)

    def test_namespace_lock_metadata_is_not_weakened(self):
        path = self.parent / '.aetherpd-installation.lock'
        path.write_bytes(b'')
        path.chmod(0o644)
        self.invoke(expected=1)
        path.chmod(0o600)
        alias = self.root / 'alias'
        os.link(path, alias)
        self.invoke(expected=1)
        alias.unlink()
        path.write_bytes(b'x')
        self.invoke(expected=1)
        path.unlink()
        path.symlink_to(self.plan)
        self.invoke(expected=1)
        self.assertFalse(self.pending.exists())

    def test_free_space_reserve_and_partial_resume_accounting(self):
        needed = 8 * 1024 * 1024 + 380 + len(self.good) + self.worker.stat().st_size
        self.invoke(fault='space:' + str(needed - 1), expected=1)
        self.assertEqual(list(self.pending.iterdir()), [])
        self.invoke(fault='space:' + str(needed))
        self.reset()
        self.invoke(fault='kill:worker-bytes', expected=-signal.SIGKILL)
        present = sum(p.stat().st_size for p in self.pending.iterdir())
        self.invoke(fault='space:' + str(needed - present))
        self.verify()

    def test_retirement_requires_matching_terminal_phase(self):
        self.invoke()
        for phase in [0, 1, 2, 3, 4, 6, 8]:
            self.phase(phase)
            self.invoke('retire', expected=1)
            self.assertTrue((self.capsule / 'worker').exists())
        changed = bytearray(self.raw_plan)
        changed[148] = ord('c')
        self.phase(5, changed)
        self.invoke('retire', expected=1)
        self.phase(7)
        self.invoke('retire')
        self.assertEqual([p for p in self.parent.iterdir() if p.name != '.aetherpd-installation.lock'], [])

    def test_process_death_during_retirement_cleanup(self):
        for point in ['retire-rename', 'retire-release', 'retire-worker', 'retire-intent']:
            with self.subTest(point=point):
                self.reset()
                self.invoke()
                self.invoke('retire', fault='kill:' + point, expected=-signal.SIGKILL)
                self.assertFalse(self.capsule.exists())
                self.invoke('open', expected=1)
                self.invoke(expected=1)  # Reconcile the old retirement first.
                self.invoke('clean')
                self.assertEqual([p for p in self.parent.iterdir() if p.name != '.aetherpd-installation.lock'], [])
                self.invoke('clean')

    def test_retired_cleanup_refuses_foreign_or_unsafe_files(self):
        self.invoke()
        self.invoke('retire', fault='kill:retire-rename', expected=-signal.SIGKILL)
        foreign = self.retired / 'foreign'
        foreign.write_bytes(b'keep')
        self.invoke('clean', expected=1)
        self.assertEqual(foreign.read_bytes(), b'keep')
        foreign.unlink()
        path = self.retired / 'worker'
        path.chmod(0o777)
        self.invoke('clean', expected=1)
        path.chmod(0o700)
        changed = bytearray(self.raw_plan)
        changed[148] = ord('c')
        self.plan.write_bytes(changed)
        self.invoke('clean', expected=1)
        self.plan.write_bytes(self.raw_plan)
        self.invoke('clean')

    def test_wrong_trust_key_cannot_open_capsule(self):
        self.invoke()
        self.key.write_bytes(bytes(32))
        self.invoke('open', expected=1)
        self.invoke('retire', expected=1)
        self.assertTrue((self.capsule / 'worker').exists())

    def test_self_binding_requires_exact_executable_inode(self):
        self.invoke()
        self.invoke('self', expected=1)
        self.invoke('self', worker=self.capsule / 'worker')

    def test_finish_preserves_recovery_until_worker_is_detached(self):
        self.invoke()
        self.invoke('arm', worker=self.units)
        recovery = self.units / 'multi-user.target.wants/aetherpd-recover.service'
        self.invoke('finish', worker=self.units, expected=1)
        self.assertTrue(recovery.is_symlink())
        self.assertTrue((self.capsule / 'worker').exists())
        self.invoke('retire')
        self.assertTrue(recovery.is_symlink())
        self.invoke('finish', worker=self.units)
        self.assertFalse(recovery.is_symlink())
        self.invoke('finish', worker=self.units)

    def test_finish_after_death_at_every_cleanup_boundary(self):
        for point in ['retire-rename', 'retire-release', 'retire-worker', 'retire-intent',
                      'disabled', 'removed:0', 'removed:1']:
            with self.subTest(point=point):
                self.invoke()
                self.invoke('arm', worker=self.units)
                if point.startswith('retire-'):
                    self.invoke('retire', fault='kill:' + point, expected=-signal.SIGKILL)
                else:
                    self.invoke('retire')
                    self.invoke('finish', worker=self.units, fault='kill:' + point, expected=-signal.SIGKILL)
                self.invoke('finish', worker=self.units)
                self.assertFalse(self.capsule.exists())
                self.assertFalse(self.retired.exists())
                self.assertEqual(list(self.units.rglob('*.service')), [])

    def test_finish_refuses_pending_or_foreign_transaction(self):
        self.invoke()
        self.invoke('arm', worker=self.units)
        self.invoke('retire', fault='kill:retire-rename', expected=-signal.SIGKILL)
        for phase in [1, 2, 3, 4, 6]:
            self.phase(phase)
            self.invoke('finish', worker=self.units, expected=1)
            self.assertTrue((self.retired / 'worker').exists())
        changed = bytearray(self.raw_plan)
        changed[148] = ord('c')
        self.phase(5, changed)
        self.invoke('finish', worker=self.units, expected=1)
        self.assertTrue((self.retired / 'worker').exists())
        self.phase(7)
        self.invoke('finish', worker=self.units)

    def test_finish_serializes_with_new_publication(self):
        self.invoke('arm', worker=self.units)
        lock = self.parent / '.aetherpd-installation.lock'
        lock.write_bytes(b'')
        lock.chmod(0o600)
        with lock.open('rb') as held:
            fcntl.flock(held, fcntl.LOCK_EX)
            self.invoke('finish', worker=self.units, expected=1)
            self.assertTrue((self.units / 'aetherpd-recover.service').exists())
        self.invoke('finish', worker=self.units)

    def test_finish_preserves_foreign_units(self):
        self.invoke()
        self.invoke('arm', worker=self.units)
        self.invoke('retire')
        foreign = self.units / 'aetherpd-recover.service'
        foreign.write_text('foreign unit\n')
        self.invoke('finish', worker=self.units, expected=1)
        self.assertEqual(foreign.read_text(), 'foreign unit\n')
        self.assertTrue((self.units / 'multi-user.target.wants/aetherpd-recover.service').is_symlink())


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--checker', required=True, type=Path)
    args, remaining = parser.parse_known_args()
    CHECKER = args.checker.resolve()
    unittest.main(argv=[__file__, *remaining])
