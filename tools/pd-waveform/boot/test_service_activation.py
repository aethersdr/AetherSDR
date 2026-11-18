#!/usr/bin/env python3
"""Service adapter ordering with simulated systemd and process attestation."""
import argparse
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
import approval
import migration
import upgrade
import units
import test_service as fixture
from test_service import NAMES, properties, text

CHECKER = None


class ServiceActivationTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='pd-service-activation-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.store = self.root/'store'
        self.store.mkdir(mode=0o700)
        self.units = self.root/'etc/systemd/system'
        self.units.mkdir(parents=True)
        for name, data in units.render('arm64',True).items():
            self.put(self.units/name, data.encode(), 0o644)
        self.plan = migration.encode(approval.encode(1, '4.3.16.42034', 'fixture-8600', bytes(32)),
                                     'a'*64, 'b'*64, bytes(range(32)))
        self.upgrade = upgrade.encode(1, '4.3.16.42034', 'fixture-8600', bytes(32), bytes([1])*32,
                                      bytes([2])*32, bytes([3])*32)
        self.put(self.root/'approved', self.upgrade)
        self.put(self.store/'migration-approved', self.plan)
        self.put(self.store/'transaction', b'PDTXN001'+self.plan+struct.pack('>I', 5))
        self.put(self.store/'transaction.lock', b'')
        self.capsule = self.root/'aetherpd-upgrade'
        self.capsule.mkdir(mode=0o700)
        self.put(self.root/'executable', b'fixture executable', 0o755)
        self.phase(3)
        for i, name in enumerate(NAMES):
            self.put(self.root/('before-'+name), text(properties(i)).encode())
            self.put(self.root/('after-'+name), text(properties(i, True)).encode())
        inspection = 'aetherpd-start@0-2-78666.service'
        for prefix in ['before-', 'after-']:
            self.put(self.root/(prefix+inspection), text(fixture.ServiceTest.session(self, inspection)).encode())
        changed = properties(2, True);changed['MainPID'] = '43'
        self.put(self.root/('changed-'+NAMES[2]), text(changed).encode())
        self.layout(True)

    def layout(self, managed):
        for name,data in units.render('arm64',managed).items():
            self.put(self.units/name,data.encode(),0o644)
        for prefix in ['before-', 'after-', 'changed-']:
            for name,legacy,arg in [(NAMES[2],'/run/aetherpd/bin/pd-primary-launch','--primary'),
                                    ('aetherpd-start@0-2-78666.service','/run/aetherpd/bin/pd-start-session','--session')]:
                path=self.root/(prefix+name)
                if path.exists():
                    old=fixture.executable(legacy);new=fixture.executable(fixture.BOOT,arg)
                    path.write_text(path.read_text().replace(old,new) if managed else path.read_text().replace(new,old))

    def put(self, path, data, mode=0o600):
        path.write_bytes(data);path.chmod(mode)

    def phase(self, phase, installation=False):
        self.put(self.store/('transaction' if installation else 'upgrade-transaction'),
                 (b'PDTXN001'+self.plan if installation else b'PDUTXN01'+self.upgrade)+struct.pack('>I', phase))

    def call(self, mode, expected=0, fault='', installation=False):
        for name in ['commands', 'events']:
            (self.root/name).unlink(missing_ok=True)
        result = subprocess.run([str(CHECKER), str(self.root), mode,
                                 'installation' if installation else 'upgrade', fault], capture_output=True, timeout=5)
        self.assertEqual(result.returncode, expected, (result.stdout, result.stderr))

    def events(self):
        path = self.root/'events'
        return path.read_text().splitlines() if path.exists() else []

    def starts(self):
        path = self.root/'commands'
        return [line for line in path.read_text().splitlines() if ' start ' in line] if path.exists() else []

    def test_candidate_and_previous_prepare_and_activate(self):
        for phase in [3, 6]:
            self.phase(phase)
            self.call('prepare')
            self.assertEqual(self.starts(), ['/usr/bin/systemctl start aetherpd-boot.service'])
            self.assertEqual(self.events(), ['start'])
            self.call('activate')
            self.assertEqual(self.starts(), ['/usr/bin/systemctl start aetherpd-shim.service'])
            self.assertEqual(self.events(), ['start', 'open', 'wait', 'check', 'close'])

    def test_bounded_control_restart_uses_existing_activation_and_rollback_gates(self):
        for name,data in units.render('arm64',True,True,True).items():
            self.put(self.units/name,data.encode(),0o644)
        for prefix in ['before-', 'after-', 'changed-']:
            path=self.root/(prefix+NAMES[2])
            path.write_text(path.read_text()+'Restart=on-failure\nRestartUSec=5s\nStartLimitIntervalUSec=1min\nStartLimitBurst=3\nStartLimitAction=none\n')
        for phase in (3,6):
            self.phase(phase);self.call('prepare');self.call('activate')
            self.assertEqual(self.starts(),['/usr/bin/systemctl start aetherpd-shim.service'])
            self.assertEqual(self.events(),['start','open','wait','check','close'])

    def test_upgrade_requires_permanent_management_layout(self):
        self.layout(False)
        for phase in [3,6]:
            self.phase(phase)
            for mode in ['prepare','activate']:
                self.call(mode,1);self.assertEqual(self.starts(),[])

    def test_all_other_phases_refuse_before_commands(self):
        for phase in [0, 1, 2, 4, 5, 7, 8]:
            self.phase(phase)
            for mode in ['prepare', 'activate']:
                self.call(mode, 1)
                self.assertFalse((self.root/'commands').exists())

    def test_worker_and_capsule_required_before_commands(self):
        for mode in ['prepare', 'activate']:
            self.call(mode, 1, 'no-worker')
            self.assertFalse((self.root/'commands').exists())
        self.capsule.rmdir()
        for mode in ['prepare', 'activate']:
            self.call(mode, 1)
            self.assertFalse((self.root/'commands').exists())

    def test_approval_and_partial_journal_refused(self):
        changed = bytearray(self.upgrade);changed[-1] ^= 1
        self.put(self.root/'approved', changed)
        for mode in ['prepare', 'activate']:
            self.call(mode, 1);self.assertEqual(self.starts(), [])
        self.put(self.root/'approved', self.upgrade)
        self.put(self.store/'upgrade-transaction.new', b'')
        for mode in ['prepare', 'activate']:
            self.call(mode, 1);self.assertEqual(self.starts(), [])

    def test_worker_loss_and_phase_change_during_start_refuse(self):
        for fault in ['worker-dies', 'phase-changes']:
            for mode in ['prepare', 'activate']:
                self.phase(3)
                self.call(mode, 1, fault)
                self.assertEqual(len(self.starts()), 1)

    def test_failed_command_cannot_be_reported_ready(self):
        for mode in ['prepare', 'activate']:
            self.call(mode, 1, 'command-fails')
            self.assertEqual(self.events(), [])

    def test_process_and_readiness_failures_close_pinned_handles(self):
        for fault in ['reject-process', 'reject-readiness', 'changed-pid', 'reject-final-process']:
            with self.subTest(fault=fault):
                self.call('activate', 1, fault)
                events = self.events()
                if fault=='reject-process':
                    self.assertNotIn('wait', events)
                else:
                    self.assertEqual(events[-1], 'close')

    def test_file_or_loaded_definition_change_prevents_start(self):
        for target in [self.units/NAMES[2], self.root/('before-'+NAMES[2])]:
            old = target.read_bytes();target.write_bytes(b'foreign')
            for mode in ['prepare', 'activate']:
                self.call(mode, 1);self.assertEqual(self.starts(), [])
            target.write_bytes(old)

    def test_definition_change_after_start_refuses(self):
        for mode in ['prepare', 'activate']:
            path = self.units/NAMES[2];old = path.read_bytes()
            self.call(mode, 1, 'unit-changes')
            self.assertEqual(len(self.starts()), 1)
            path.write_bytes(old)

    def test_boot_and_socket_must_be_active_after_start(self):
        for kind in [0, 1]:
            path = self.root/('after-'+NAMES[kind]);old = path.read_bytes()
            path.write_text(text(properties(kind)))
            self.call('activate', 1)
            if kind==0:
                self.call('prepare', 1)
            path.write_bytes(old)

    def test_original_installation_activation_still_works(self):
        self.layout(False)
        self.put(self.root/'approved', self.plan)
        self.phase(4, installation=True)
        for mode in ['prepare', 'activate']:
            self.call(mode, installation=True)
        for phase in [1, 2, 3, 5, 6, 7]:
            self.phase(phase, installation=True)
            self.call('activate', 1, installation=True)
            self.assertEqual(self.starts(), [])


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--checker', required=True, type=Path)
    args, rest = p.parse_known_args()
    CHECKER = args.checker.resolve()
    unittest.main(argv=[__file__, *rest])
