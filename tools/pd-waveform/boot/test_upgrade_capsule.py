#!/usr/bin/env python3
"""Protected upgrade inputs, interrupted publication and terminal removal."""
import argparse
import hashlib
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest
import upgrade
import bundle
import release
import test_upgrade

CHECKER = None
WORKER = None


class UpgradeCapsuleTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='pd-upgrade-capsule-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.store = self.root/'store';self.store.mkdir(mode=0o700)
        self.active = self.root/'aetherpd-upgrade'
        self.pending = self.root/'.aetherpd-upgrade'
        self.retired = self.root/'.aetherpd-upgrade-retired'
        self.staging = self.root/'.aetherpd-upgrade-staging'
        self.units = self.root/'units';self.units.mkdir(mode=0o755)
        self.raw = upgrade.encode(1, '4.3.16.42034', 'fixture-8600', bytes(32), bytes([1])*32,
                                  bytes([2])*32, bytes([3])*32)
        self.write(self.root/'approved', self.raw)
        self.write(self.root/'key', bytes([3])*32)
        self.expected = b'fixture'.ljust(64, b'\0')+b'100.64.0.1'.ljust(16, b'\0')+b'fixture.example'.ljust(256, b'\0')
        self.write(self.root/'expected', self.expected)
        self.write(self.root/'worker-input', (WORKER or CHECKER).read_bytes(), 0o700)
        self.assertLessEqual((self.root/'worker-input').stat().st_size, 512*1024)

    def write(self, path, data, mode=0o600):
        path.write_bytes(data);path.chmod(mode)

    def call(self, mode='publish', expected=0, fault='', executable=None):
        result = subprocess.run([str(executable or CHECKER), str(self.root), mode, fault],
                                capture_output=True, timeout=5)
        self.assertEqual(result.returncode, expected, (result.stdout, result.stderr))

    def phase(self, phase, approved=None):
        self.write(self.store/'upgrade-transaction', b'PDUTXN01'+(approved or self.raw)+struct.pack('>I', phase))

    def snapshot(self, path):
        if not path.exists():
            return None
        return {str(p.relative_to(path)): (p.lstat().st_mode, p.lstat().st_ino,
                p.read_bytes() if p.is_file() and not p.is_symlink() else None)
                for p in path.rglob('*')}

    def test_publish_open_repeat_and_exact_inventory(self):
        self.call();before = self.snapshot(self.active)
        self.call();self.call('open')
        self.assertEqual(before, self.snapshot(self.active))
        self.assertEqual(set(before), {'worker', 'intent'})
        self.assertEqual((self.active/'worker').stat().st_mode & 0o7777, 0o700)
        intent = (self.active/'intent').read_bytes()
        self.assertEqual(intent, b'PDUPCAP1'+self.raw+hashlib.blake2b((self.root/'worker-input').read_bytes(), digest_size=32).digest()+self.expected)
        self.assertFalse(self.pending.exists())

    def signed_origin(self, rolled_back=False):
        fixture=test_upgrade.UpgradeTest();fixture.setUp();self.addCleanup(fixture.doCleanups)
        self.raw=fixture.approved.read_bytes();self.write(self.root/'approved',self.raw)
        self.write(self.root/'key',fixture.key.read_bytes())
        prior=b'PDUTXN01'+fixture.good+struct.pack('>I',7) if rolled_back else bytes(364)
        header=fixture.artifacts[int(rolled_back)][:release.PREFIX]
        origin=b'PDORIG01'+bundle.blake(self.raw)+prior+header
        self.write(self.root/'origin',origin)
        return origin

    def test_version2_carries_initial_management_and_cleans(self):
        origin=self.signed_origin();self.call('publish-origin')
        intent=(self.active/'intent').read_bytes()
        self.assertEqual(intent,b'PDUPCAP2'+self.raw+bundle.blake((self.root/'worker-input').read_bytes())+self.expected+origin)
        self.call('publish-origin');self.call('open');self.call('begin');self.call('begin-retry')
        self.assertEqual((self.active/'started').read_bytes(),b'PDUPRUN1'+bundle.blake(intent))
        self.phase(4);self.call('retire');self.assertFalse(self.active.exists());self.assertFalse(self.retired.exists())

    def test_version2_carries_rolled_back_management_key(self):
        self.signed_origin(True);self.call('publish-origin');self.call('open');self.call('begin')
        self.phase(7);self.call('retire');self.assertFalse(self.active.exists())

    def test_version2_interrupted_publication_resumes(self):
        origin=self.signed_origin()
        intent=b'PDUPCAP2'+self.raw+bundle.blake((self.root/'worker-input').read_bytes())+self.expected+origin
        for length in [0,8,39,40,727,728,len(intent)-1,len(intent)]:
            self.pending.mkdir(mode=0o700)
            self.write(self.pending/'intent.new',intent[:length]);self.call('publish-origin');self.call('open')
            shutil.rmtree(self.active)

    def test_version2_foreign_origin_refuses_without_publication(self):
        origin=self.signed_origin(True)
        for offset in [0,8,40,403,len(origin)-1]:
            bad=bytearray(origin);bad[offset]^=1;self.write(self.root/'origin',bad)
            self.call('publish-origin',expected=1);self.assertFalse(self.active.exists());self.assertFalse(self.pending.exists())

    def test_version2_cannot_replace_existing_legacy_capsule(self):
        self.signed_origin();self.call();before=self.snapshot(self.active)
        self.call('publish-origin',expected=1);self.assertEqual(before,self.snapshot(self.active))

    def test_version2_terminal_cleanup_after_worker_removal(self):
        self.signed_origin();self.call('publish-origin');self.call('begin');self.phase(7)
        self.call('retire',expected=99,fault='upgrade-retire-worker')
        self.call('cleanup');self.assertFalse(self.retired.exists())

    def test_armed_publication_is_recoverable_at_every_boundary(self):
        points=['upgrade-intent-durable','worker-bytes','worker-file','worker-rename',
                'upgrade-capsule-durable','unit-file:0','unit-rename:0','unit-file:1',
                'unit-rename:1','link-staged','link-published','armed',
                'upgrade-recovery-armed','upgrade-capsule-rename']
        for point in points:
            with self.subTest(point=point):
                self.setUp();self.signed_origin();self.call('publish-armed',expected=99,fault=point)
                self.assertFalse(self.pending.exists())
                self.assertEqual(self.active.exists(),point=='upgrade-capsule-rename')
                if self.active.exists():
                    self.call('verify-units');self.call('open')
                self.call('publish-armed');self.call('verify-units');self.call('open')
                self.assertFalse(self.staging.exists())

    def test_armed_publication_rearms_after_inert_stage_cleanup(self):
        self.signed_origin();self.phase(7)
        self.call('publish-armed',expected=99,fault='upgrade-recovery-armed')
        before=self.snapshot(self.staging)
        self.call('cleanup-units');self.call('verify-units',expected=1)
        self.assertEqual(before,self.snapshot(self.staging))
        self.call('publish-armed');self.call('verify-units');self.call('open')

    def test_foreign_recovery_unit_never_publishes_active_authority(self):
        self.signed_origin();foreign=self.units/'aetherpd-upgrade-recover.service'
        self.write(foreign,b'foreign',0o644)
        self.call('publish-armed',expected=1)
        self.assertFalse(self.active.exists());self.assertEqual(foreign.read_bytes(),b'foreign')
        foreign.unlink();self.call('publish-armed');self.call('verify-units')

    def test_older_cleanup_busy_refuses_before_arming_or_publication(self):
        self.signed_origin();self.call('publish-armed',expected=1,fault='cleanup-busy')
        self.assertFalse(self.active.exists());self.assertTrue(self.staging.exists())
        self.assertEqual(list(self.units.iterdir()),[])
        self.call('publish-armed');self.call('verify-units');self.call('open')

    def test_old_pending_authority_is_not_displaced(self):
        self.signed_origin();self.pending.mkdir(mode=0o700)
        self.write(self.pending/'intent.new',b'foreign');before=self.snapshot(self.pending)
        self.call('publish-armed',expected=1)
        self.assertEqual(before,self.snapshot(self.pending));self.assertFalse(self.active.exists())
        self.assertEqual(list(self.units.iterdir()),[])

    def test_armed_staging_refuses_foreign_worker_and_unsafe_directory(self):
        self.signed_origin();self.staging.symlink_to('missing')
        self.call('publish-armed',expected=1);self.staging.unlink()
        self.staging.mkdir(mode=0o755);self.call('publish-armed',expected=1)
        self.staging.chmod(0o700);self.write(self.staging/'worker.new',b'foreign')
        before=self.snapshot(self.staging);self.call('publish-armed',expected=1)
        self.assertEqual(before,self.snapshot(self.staging));self.assertFalse(self.active.exists())
        self.assertEqual(list(self.units.iterdir()),[])

    def held(self,mode,point):
        child=subprocess.Popen([str(CHECKER),str(self.root),mode,'hold:'+point],
                               stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        def finish():
            if child.poll() is None:
                child.kill()
            child.communicate(timeout=5)
        self.addCleanup(finish)
        self.assertEqual(child.stdout.readline(),b'ready\n')
        return child

    def test_terminal_cleanup_cannot_disarm_publication_in_progress(self):
        self.signed_origin();self.phase(7)
        child=self.held('publish-armed','upgrade-recovery-armed')
        self.call('verify-units');before=self.snapshot(self.units)
        self.call('cleanup-units',expected=1)
        self.assertEqual(before,self.snapshot(self.units));self.assertFalse(self.active.exists())
        child.communicate(b'continue',timeout=5);self.assertEqual(child.returncode,0)
        self.call('verify-units');self.call('open');self.call('cleanup-units',expected=1)

    def test_publication_cannot_race_terminal_unit_removal(self):
        self.signed_origin();self.phase(7)
        self.call('publish-armed',expected=99,fault='upgrade-recovery-armed')
        child=self.held('cleanup-units','disabled')
        self.call('publish-armed',expected=1);self.assertFalse(self.active.exists())
        child.communicate(b'continue',timeout=5);self.assertEqual(child.returncode,0)
        self.call('publish-armed');self.call('verify-units');self.call('open')

    def test_repeated_active_publication_checks_identity_before_arming(self):
        self.signed_origin();self.call('publish-armed')
        before=self.snapshot(self.active);shutil.rmtree(self.units);self.units.mkdir(mode=0o755)
        self.write(self.root/'expected',b'other'.ljust(64,b'\0')+self.expected[64:])
        self.call('publish-armed',expected=1);self.assertEqual(list(self.units.iterdir()),[])
        self.assertEqual(before,self.snapshot(self.active));self.write(self.root/'expected',self.expected)
        self.call('publish-armed');self.call('verify-units')

    def test_real_executable_inode_binding(self):
        self.call()
        self.call('self', 1)
        self.call('self', executable=self.active/'worker')
        copied = self.root/'copied-worker'
        shutil.copyfile(self.active/'worker', copied);copied.chmod(0o700)
        self.call('self', 1, executable=copied)
    def test_started_receipt_is_durable_once_and_bound_to_intent(self):
        self.call();self.call('begin');self.call('begin-retry')
        receipt=self.active/'started'
        self.assertEqual(receipt.read_bytes(),b'PDUPRUN1'+hashlib.blake2b((self.active/'intent').read_bytes(),digest_size=32).digest())
        original=receipt.read_bytes();receipt.write_bytes(original[:-1]+bytes([original[-1]^1]));self.call('open',1)
        self.assertNotEqual(receipt.read_bytes(),original)
    def test_each_started_prefix_is_a_retry(self):
        self.call();expected=b'PDUPRUN1'+hashlib.blake2b((self.active/'intent').read_bytes(),digest_size=32).digest()
        for length in range(len(expected)+1):
            self.write(self.active/'started',expected[:length]);self.call('begin-retry')
            self.assertEqual((self.active/'started').read_bytes(),expected)
    def test_death_after_started_publication_is_a_retry(self):
        self.call();self.call('begin',99,'upgrade-started-durable');self.call('begin-retry')

    def test_configured_key_is_independent_authority(self):
        self.write(self.root/'key', bytes([9])*32)
        self.call(expected=1)
        self.assertFalse(self.active.exists());self.assertFalse(self.pending.exists())
        self.write(self.root/'key', bytes([3])*32);self.call()
        self.write(self.root/'key', bytes([9])*32);self.call('open', 1)

    def test_invalid_public_identity_refused_before_publication(self):
        for offset, value in [(0, ord('/')), (63, 1), (64, ord('9')), (79, 1), (80, ord('/')), (335, 1)]:
            changed = bytearray(self.expected);changed[offset] = value
            self.write(self.root/'expected', changed)
            self.call(expected=1)
            self.assertFalse(self.pending.exists());self.assertFalse(self.active.exists())

    def test_publication_crashes_resume_without_replacement(self):
        points = ['intent-bytes', 'intent-file', 'intent-rename', 'upgrade-intent-durable',
                  'worker-bytes', 'worker-file', 'worker-rename', 'upgrade-capsule-durable', 'upgrade-capsule-rename']
        for point in points:
            with self.subTest(point=point):
                self.setUp();self.call(expected=99, fault=point)
                self.call();self.call('open')
                self.assertFalse(self.pending.exists())

    def test_every_intent_prefix_resumes(self):
        self.call();intent = (self.active/'intent').read_bytes()
        shutil.rmtree(self.active)
        for length in range(len(intent)+1):
            with self.subTest(length=length):
                self.pending.mkdir(mode=0o700)
                self.write(self.pending/'intent.new', intent[:length])
                self.call();self.call('open')
                shutil.rmtree(self.active)

    def test_partial_worker_prefix_resumes_and_foreign_prefix_is_preserved(self):
        self.call();intent = (self.active/'intent').read_bytes();worker = (self.active/'worker').read_bytes()
        shutil.rmtree(self.active)
        self.pending.mkdir(mode=0o700)
        self.write(self.pending/'intent', intent)
        self.write(self.pending/'worker.new', worker[:17000])
        self.call();self.call('open');shutil.rmtree(self.active)
        self.pending.mkdir(mode=0o700);self.write(self.pending/'worker.new', b'foreign')
        before = self.snapshot(self.pending);self.call(expected=1)
        self.assertEqual(self.snapshot(self.pending), before)
        self.assertFalse((self.pending/'intent').exists())

    def test_space_reserve_refuses_before_copy(self):
        self.call(expected=1, fault='no-space')
        self.assertFalse(self.active.exists())
        self.assertEqual(list(self.pending.iterdir()), [])
        self.call()

    def test_worker_size_and_unsafe_source_refuse_before_publication(self):
        worker = self.root/'worker-input';original = worker.read_bytes()
        for data in [b'', b'x'*(512*1024+1)]:
            worker.write_bytes(data);self.call(expected=1)
            self.assertFalse(self.pending.exists())
        worker.write_bytes(original)
        worker.chmod(0o777);self.call(expected=1);worker.chmod(0o700)
        alias = self.root/'worker-alias';os.link(worker, alias);self.call(expected=1)
        alias.unlink();self.call()

    def test_unsafe_directories_and_namespace_lock_refuse(self):
        self.root.chmod(0o777);self.call(expected=1);self.root.chmod(0o700)
        lock = self.root/'.aetherpd-upgrade.lock'
        self.write(lock, b'foreign');self.call(expected=1)
        self.assertEqual(lock.read_bytes(), b'foreign');lock.unlink()
        lock.symlink_to('missing');self.call(expected=1);lock.unlink()
        self.pending.symlink_to('missing');self.call(expected=1);self.pending.unlink()
        self.pending.mkdir(mode=0o755);self.call(expected=1);self.pending.rmdir()
        self.active.symlink_to('missing');self.call(expected=1);self.active.unlink()
        self.call();self.active.chmod(0o755);self.call('open', 1)

    def test_changed_worker_identity_or_approval_cannot_replace_active(self):
        self.call();before = self.snapshot(self.active)
        for name, data in [('worker-input', b'other worker'), ('expected', b'changed'.ljust(64,b'\0')+self.expected[64:]),
                           ('approved', self.raw[:100]+bytes([9])+self.raw[101:])]:
            path = self.root/name;old = path.read_bytes();path.write_bytes(data)
            self.call(expected=1);self.assertEqual(self.snapshot(self.active), before)
            path.write_bytes(old)

    def test_corrupt_worker_and_intent_refuse_open(self):
        self.call()
        for name in ['worker', 'intent']:
            path = self.active/name;old = path.read_bytes()
            changed = bytearray(old);changed[len(changed)//2] ^= 1;path.write_bytes(changed)
            self.call('open', 1);self.assertEqual(path.read_bytes(), changed)
            path.write_bytes(old)

    def test_unsafe_file_metadata_and_links_refuse(self):
        self.call();self.call('begin')
        for name in ['worker', 'intent', 'started']:
            path = self.active/name;mode = path.stat().st_mode;data = path.read_bytes()
            for changed in [0o777, 0o4700, 0o644]:
                path.chmod(changed);self.call('open', 1)
            path.chmod(mode)
            alias = self.root/'alias';os.link(path, alias);self.call('open', 1);alias.unlink()
            path.unlink();self.write(alias, data, mode);path.symlink_to(alias);self.call('open', 1)
            path.unlink();alias.unlink();os.mkfifo(path, 0o600);self.call('open', 1)
            path.unlink();self.write(path, data, mode)
        self.call('open')

    def test_unknown_and_partial_active_entries_are_preserved(self):
        self.call()
        for name in ['foreign', 'worker.new', 'intent.new']:
            path = self.active/name;self.write(path, b'foreign')
            before = self.snapshot(self.active)
            self.call('open', 1);self.call(expected=1)
            self.assertEqual(self.snapshot(self.active), before);path.unlink()

    def test_namespace_lease_blocks_concurrent_mutations(self):
        self.call();self.phase(4)
        child = subprocess.Popen([str(CHECKER), str(self.root), 'hold', ''], stdin=subprocess.PIPE,
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            self.assertEqual(child.stdout.readline(), b'ready\n')
            self.call(expected=1);self.call('open', 1);self.call('retire', 1)
            child.kill();child.communicate(timeout=5)
            self.call('open');self.call('retire')
        finally:
            if child.poll() is None:
                child.kill()
            child.communicate(timeout=5)

    def test_retirement_only_after_matching_terminal_phase(self):
        self.call();before = self.snapshot(self.active)
        for phase in [0, 1, 2, 3, 5, 6, 8]:
            self.phase(phase);self.call('retire', 1)
            self.assertEqual(self.snapshot(self.active), before)
        changed = bytearray(self.raw);changed[100] ^= 1
        self.phase(4, changed);self.call('retire', 1)
        self.phase(4);self.call('retire')
        self.assertFalse(self.active.exists());self.assertFalse(self.retired.exists())
        self.call('cleanup');self.call('cleanup')

    def test_terminal_partial_journal_cannot_detach_worker(self):
        self.call();self.phase(4);self.write(self.store/'upgrade-transaction.new', b'')
        self.call('retire', 1);self.assertTrue(self.active.exists())

    def test_retirement_crashes_finish_from_terminal_record(self):
        for phase in [4, 7]:
            for point in ['upgrade-retire-rename', 'upgrade-retire-started', 'upgrade-retire-worker', 'upgrade-retire-intent']:
                with self.subTest(phase=phase, point=point):
                    self.setUp();self.call();self.call('begin');self.phase(phase)
                    self.call('retire', 99, point)
                    self.assertFalse(self.active.exists());self.assertTrue(self.retired.exists())
                    self.call('cleanup');self.call('cleanup')
                    self.assertFalse(self.retired.exists())

    def test_foreign_retired_entries_or_changed_approval_are_preserved(self):
        self.call();self.phase(4);self.call('retire', 99, 'upgrade-retire-rename')
        changed = bytearray(self.raw);changed[100] ^= 1
        self.phase(4, changed);before = self.snapshot(self.retired)
        self.call('cleanup', 1);self.assertEqual(self.snapshot(self.retired), before)
        self.phase(4);self.write(self.retired/'foreign', b'foreign');before = self.snapshot(self.retired)
        self.call('cleanup', 1);self.assertEqual(self.snapshot(self.retired), before)

    def test_active_authority_blocks_detached_cleanup(self):
        self.call();self.phase(4);self.call('cleanup', 1)
        self.assertTrue((self.active/'worker').exists())

    def test_old_retired_state_blocks_new_publication(self):
        self.retired.mkdir(mode=0o700);self.call(expected=1)
        self.assertFalse(self.active.exists());self.assertFalse(self.pending.exists())


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--checker', required=True, type=Path)
    p.add_argument('--worker-executable', type=Path)
    args, rest = p.parse_known_args()
    CHECKER = args.checker.resolve();WORKER = args.worker_executable.resolve() if args.worker_executable else None
    unittest.main(argv=[__file__, *rest])
