#!/usr/bin/env python3
"""Fixed recovery-unit publication/removal against disposable filesystem state."""
import argparse
import os
from pathlib import Path
import shutil
import signal
import struct
import subprocess
import tempfile
import unittest
import approval
import migration
import units
import upgrade_units
import upgrade

CHECKER = None


class UpgradeUnitsTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='pd-worker-units-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.units = self.root / 'units'; self.units.mkdir(mode=0o755)
        self.store = self.root / 'store'; self.store.mkdir(mode=0o755)
        self.plan = self.root / 'plan'
        self.raw = upgrade.encode(1,'4.3.16.42034','fixture-8600',bytes(32),bytes([1])*32,bytes([2])*32,bytes([3])*32)
        self.plan.write_bytes(self.raw); self.plan.chmod(0o600)
        self.phase(4)
        self.definitions = upgrade_units.render('arm64')
        self.wants = self.units / 'multi-user.target.wants'

    def phase(self, phase):
        p = self.store / 'upgrade-transaction'
        p.write_bytes(b'PDUTXN01' + self.raw + struct.pack('>I', phase)); p.chmod(0o600)

    def invoke(self, command='arm', fault='', expected=0):
        p = subprocess.run([str(CHECKER), command, str(self.units), str(self.store), str(self.plan), fault],
                           capture_output=True, timeout=5)
        self.assertEqual(p.returncode, expected, p.stderr.decode(errors='replace'))

    def verify(self):
        self.invoke('verify')
        for name, text in self.definitions.items():
            p = self.units / name
            self.assertFalse(p.is_symlink()); self.assertEqual(p.read_text(), text)
            self.assertEqual(p.stat().st_mode & 0o7777, 0o644)
        self.assertEqual(os.readlink(self.wants / 'aetherpd-upgrade-recover.service'), '../aetherpd-upgrade-recover.service')
        self.assertFalse(os.path.lexists(self.wants / 'aetherpd-upgrade.service'))

    def reset(self):
        shutil.rmtree(self.units); self.units.mkdir(mode=0o755)

    def test_arm_verify_and_idempotency(self):
        self.invoke(); self.verify()
        inode = (self.units / 'aetherpd-upgrade-recover.service').stat().st_ino
        self.invoke(); self.verify()
        self.assertEqual((self.units / 'aetherpd-upgrade-recover.service').stat().st_ino, inode)

    def test_interrupted_publication_resumes(self):
        for point in ['unit-file:0', 'unit-rename:0', 'unit-file:1', 'unit-rename:1', 'link-staged', 'link-published', 'armed']:
            with self.subTest(point=point):
                self.reset(); self.invoke(fault='kill:' + point, expected=-signal.SIGKILL)
                self.invoke(); self.verify()

    def test_foreign_unit_refuses_before_other_publication(self):
        p = self.units / 'aetherpd-upgrade-recover.service'; p.write_text('foreign'); p.chmod(0o644)
        self.invoke(expected=1)
        self.assertEqual(list(self.units.iterdir()), [p]); self.assertEqual(p.read_text(), 'foreign')

    def test_foreign_enablement_is_preserved(self):
        self.wants.mkdir(); link = self.wants / 'aetherpd-upgrade-recover.service'; link.symlink_to('/foreign')
        self.invoke(expected=1); self.assertEqual(os.readlink(link), '/foreign')
        self.assertFalse((self.units / 'aetherpd-upgrade.service').exists())

    def test_forward_boot_enablement_is_refused(self):
        self.wants.mkdir(); (self.wants / 'aetherpd-upgrade.service').symlink_to('../aetherpd-upgrade.service')
        self.invoke(expected=1)
        self.assertFalse((self.units / 'aetherpd-upgrade-recover.service').exists())

    def test_unit_dropins_are_preserved_and_refused(self):
        p = self.units / 'aetherpd-upgrade-recover.service.d'; p.mkdir()
        (p / 'override.conf').write_text('[Service]\nExecStart=/foreign\n')
        self.invoke(expected=1); self.assertEqual(list(self.units.iterdir()), [p])

    def test_unsafe_unit_files_refused(self):
        self.invoke()
        p = self.units / 'aetherpd-upgrade-recover.service'
        p.chmod(0o666); self.invoke(expected=1); self.invoke('verify', expected=1)
        p.chmod(0o644); alias = self.root / 'alias'; os.link(p, alias)
        self.invoke(expected=1); alias.unlink()
        text = p.read_text(); p.unlink(); alias.write_text(text); p.symlink_to(alias)
        self.invoke(expected=1)

    def test_foreign_partial_is_not_overwritten(self):
        p = self.units / '.aetherpd-upgrade.service.new'; p.write_bytes(b'foreign'); p.chmod(0o600)
        self.invoke(expected=1); self.assertEqual(p.read_bytes(), b'foreign')

    def test_matching_prefix_can_resume(self):
        p = self.units / '.aetherpd-upgrade.service.new'
        p.write_bytes(self.definitions['aetherpd-upgrade.service'].encode()[:20]); p.chmod(0o600)
        self.invoke(); self.verify(); self.assertFalse(p.exists())

    def test_disarm_requires_terminal_state(self):
        self.invoke()
        for phase in [0, 1, 2, 3, 5, 6, 8]:
            self.phase(phase); self.invoke('disarm', expected=1); self.verify()
        self.phase(7); self.invoke('disarm')
        self.assertEqual(list(self.units.iterdir()), [self.wants]); self.assertEqual(list(self.wants.iterdir()), [])

    def test_interrupted_disarm_resumes_and_preserves_other_units(self):
        for point in ['disabled', 'removed:0', 'removed:1']:
            self.reset(); self.invoke(); foreign = self.units / 'other.service'; foreign.write_text('keep')
            self.invoke('disarm', fault='kill:' + point, expected=-signal.SIGKILL)
            self.invoke('disarm'); self.invoke('disarm')
            self.assertEqual(foreign.read_text(), 'keep')
            self.assertEqual(sorted(p.name for p in self.units.iterdir()), ['multi-user.target.wants', 'other.service'])

    def test_changed_unit_prevents_destructive_cleanup(self):
        self.invoke(); p = self.units / 'aetherpd-upgrade-recover.service'; p.write_text('changed')
        self.invoke('disarm', expected=1)
        self.assertTrue((self.wants / 'aetherpd-upgrade-recover.service').is_symlink())
        self.assertTrue((self.units / 'aetherpd-upgrade.service').exists())
        self.assertEqual(p.read_text(), 'changed')

    def test_unwritable_and_symlinked_parent_shapes_refuse(self):
        self.units.chmod(0o777); self.invoke(expected=1); self.units.chmod(0o755)
        alias = self.root / 'alias'; alias.symlink_to(self.units)
        self.units = alias; self.invoke(expected=1)

    def test_cleanup_requires_matching_approval_and_absent_authority(self):
        self.invoke()
        original=self.plan.read_bytes();changed=bytearray(original);changed[-1]^=1
        self.plan.write_bytes(changed);self.invoke('disarm',expected=1);self.verify();self.plan.write_bytes(original)
        for name in ['aetherpd-upgrade','.aetherpd-upgrade','.aetherpd-upgrade-retired']:
            capsule=self.root/name;capsule.mkdir(mode=0o700)
            self.invoke('disarm',expected=1);self.verify();capsule.rmdir()
        partial=self.store/'upgrade-transaction.new';partial.write_bytes(b'')
        self.invoke('disarm',expected=1);self.verify();partial.unlink()
        for phase in [4,7]:
            self.phase(phase);self.invoke('disarm');self.invoke('disarm');self.invoke()

    def test_boot_terminal_cleanup_retires_empty_detached_capsule_and_units(self):
        for phase in [4,7]:
            self.phase(phase);self.invoke()
            retired=self.root/'.aetherpd-upgrade-retired';retired.mkdir(mode=0o700)
            self.invoke('reconcile');self.assertFalse(retired.exists())
            self.assertFalse((self.units/'aetherpd-upgrade.service').exists())
            self.invoke('reconcile')

    def test_boot_cleanup_refuses_active_authority_or_wrong_configured_key(self):
        self.invoke();capsule=self.root/'aetherpd-upgrade';capsule.mkdir(mode=0o700)
        self.invoke('reconcile',expected=1);self.verify();capsule.rmdir()
        original=self.plan.read_bytes();changed=bytearray(original);changed[-1]^=1
        self.plan.write_bytes(changed);self.invoke('reconcile',expected=1);self.verify();self.plan.write_bytes(original)
        retired=self.root/'.aetherpd-upgrade-retired';retired.mkdir(mode=0o700);(retired/'foreign').write_text('preserve')
        self.invoke('reconcile',expected=1);self.verify();self.assertEqual((retired/'foreign').read_text(),'preserve')

    def test_boot_leaves_nonterminal_recovery_armed(self):
        self.invoke()
        for phase in [1,2,3,5,6]:
            self.phase(phase);self.invoke('reconcile');self.verify();self.invoke('finish',expected=1)
        (self.store/'upgrade-transaction').unlink();self.invoke('reconcile');self.verify()
        (self.store/'upgrade-transaction').write_bytes(b'foreign');self.invoke('reconcile',expected=1);self.verify()

    def test_boot_cleanup_resumes_after_unit_removal_interruption(self):
        for point in ['disabled','removed:0','removed:1']:
            self.reset();self.phase(4);self.invoke()
            self.invoke('reconcile',fault='kill:'+point,expected=-signal.SIGKILL)
            self.invoke('reconcile');self.invoke('reconcile')
            self.assertFalse((self.units/'aetherpd-upgrade.service').exists())


class UpgradeOrderingTest(unittest.TestCase):
    def test_recovery_can_start_runtime_without_ordering_deadlock(self):
        for arch in ['arm64','armv7']:
            definitions=upgrade_units.render(arch)
            forward=definitions['aetherpd-upgrade.service'];recovery=definitions['aetherpd-upgrade-recover.service']
            parent='/user' if arch=='armv7' else '/var/lib/smartsdr'
            self.assertIn('ExecStart='+parent+'/aetherpd-upgrade/worker\n',forward)
            self.assertIn('ExecStart='+parent+'/aetherpd-upgrade/worker --recover\n',recovery)
            self.assertNotIn('Before=',forward);self.assertNotIn('Before=',recovery)
            self.assertNotIn('WantedBy=',forward);self.assertIn('WantedBy=multi-user.target',recovery)
            self.assertNotIn('/pd-boot',forward+recovery)

    @unittest.skipUnless(shutil.which('systemd-analyze'), 'requires Linux systemd verifier')
    def test_boot_recovery_and_runtime_graph_has_no_ordering_cycle(self):
        with tempfile.TemporaryDirectory() as path:
            root = Path(path)
            for name, text in {**units.render('arm64',True), **upgrade_units.render('arm64')}.items():
                lines = [line.split('=', 1)[0] + '=' + shutil.which('true')
                         if line.startswith(('ExecStart=', 'ExecStartPost=')) else line for line in text.splitlines()]
                (root / name).write_text('\n'.join(lines) + '\n')
            for name in ['smartsdr.service', 'podman-restart.service']:
                (root / name).write_text('[Service]\nType=oneshot\nExecStart=' + shutil.which('true') + '\n')
            (root / 'default.target').write_text('[Unit]\nRequires=basic.target aetherpd-upgrade-recover.service aetherpd-shim.service\nAfter=basic.target aetherpd-shim.service\n')
            env = dict(os.environ, SYSTEMD_UNIT_PATH=str(root) + ':/usr/lib/systemd/system')
            p = subprocess.run(['systemd-analyze', 'verify', '--man=no', '--generators=no', 'default.target'],
                               env=env, capture_output=True, text=True, timeout=20)
            self.assertEqual(p.returncode, 0, p.stderr)
            self.assertNotIn('ordering cycle', p.stderr.lower(), p.stderr)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--checker', required=True, type=Path)
    args, remaining = parser.parse_known_args(); CHECKER = args.checker.resolve()
    unittest.main(argv=[__file__, *remaining])
