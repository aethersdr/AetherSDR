#!/usr/bin/env python3
"""Retire original slots only after durable generation selection."""
import argparse
import os
from pathlib import Path
import signal
import unittest
import test_generation
import test_retained
import test_upgrade
import units

class SlotsRetireTest(test_retained.RetainedTest):
    def test_original_slots_retire_for_both_terminal_choices(self):
        for committed in [False,True]:
            with self.subTest(committed=committed):
                self.setUp();self.terminal_pair(committed);selected=self.snapshot(self.selected)
                private=self.store/'state';private.mkdir(mode=0o700);self.fixture.write(private/'identity',b'private identity')
                identity=self.snapshot(private)
                metadata={n:(self.store/n).read_bytes() for n in ['installed','active','transaction','migration-approved']}
                self.invoke('slots-retire');self.assertEqual(list((self.store/'slots').iterdir()),[])
                self.assertEqual(selected,self.snapshot(self.selected));self.invoke('active')
                self.assertEqual(identity,self.snapshot(private))
                for n,data in metadata.items():self.assertEqual((self.store/n).read_bytes(),data)
                before=self.snapshot(self.store);self.invoke('slots-retire');self.assertEqual(before,self.snapshot(self.store))
                (self.store/'upgrade-generation').unlink();self.invoke('active',expected=1)

    def test_every_slot_retirement_boundary_resumes(self):
        points=['slots-retired-bytes','slots-retired-file','slots-retired-rename','slots-durable:0',
                'slots-removed:0','slots-removed:1','slots-removed:2']
        for point in points:
            with self.subTest(point=point):
                self.setUp();self.terminal_pair();before=self.snapshot(self.selected)
                self.invoke('slots-retire',expected=-signal.SIGKILL,fault=point)
                self.invoke('active');self.invoke('slots-retire');self.assertEqual(before,self.snapshot(self.selected))
                self.assertEqual(list((self.store/'slots').iterdir()),[])

    def test_all_three_authenticated_original_slots_are_removed(self):
        self.terminal_pair();payload=(self.store/'slots/A.bundle').read_bytes()
        self.fixture.write(self.store/'slots/B.bundle',payload);self.fixture.write(self.store/'slots/C.bundle',payload)
        active=bytearray((self.store/'active').read_bytes());active[48]=ord('B');active[56:88]=active[16:48]
        self.fixture.write(self.store/'active',active);self.invoke('slots-retire')
        self.assertEqual(list((self.store/'slots').iterdir()),[])

    def test_unknown_files_and_corrupt_payload_remain_untouched(self):
        self.terminal_pair();slots=self.store/'slots'
        for name in ['unknown','incoming.new']:
            self.fixture.write(slots/name,b'foreign');before=self.snapshot(self.store)
            self.invoke('slots-retire',expected=1);self.assertEqual(before,self.snapshot(self.store));(slots/name).unlink()
        p=slots/'A.bundle';p.write_bytes(b'foreign');before=self.snapshot(self.store)
        self.invoke('slots-retire',expected=1);self.assertEqual(before,self.snapshot(self.store))

    def test_receipt_and_installed_identity_tampering_refuse(self):
        self.terminal_pair();self.invoke('slots-retire',expected=-signal.SIGKILL,fault='slots-durable:0')
        path=self.store/'slots-retired';data=path.read_bytes()
        for offset in [0,12,360,408,500,len(data)-1]:
            changed=bytearray(data);changed[offset]^=1;path.write_bytes(changed);before=self.snapshot(self.store)
            self.invoke('slots-retire',expected=1);self.assertEqual(before,self.snapshot(self.store))
        path.write_bytes(data);self.fixture.write(self.store/'slots-retired.new',b'');self.invoke('slots-retire',expected=1)
        (self.store/'slots-retired.new').unlink()
        path=self.store/'installed';changed=bytearray(path.read_bytes());changed[-1]^=1;path.write_bytes(changed)
        before=self.snapshot(self.store);self.invoke('slots-retire',expected=1);self.assertEqual(before,self.snapshot(self.store))

    def test_requires_live_terminal_authority_and_complete_selected_payload(self):
        self.terminal_pair();self.invoke('slots-retire',expected=1,fault='no-lock')
        self.begin(3);self.invoke('slots-retire',expected=1);self.begin(4)
        self.fixture.write(self.store/'active.new',b'');self.invoke('slots-retire',expected=1);(self.store/'active.new').unlink()
        (self.selected/'payload.bundle').unlink();self.invoke('slots-retire',expected=1)
        self.assertTrue((self.store/'slots/A.bundle').exists());self.assertFalse((self.store/'slots-retired').exists())

    def test_missing_original_before_receipt_is_not_assumed_retired(self):
        self.terminal_pair();(self.store/'slots/A.bundle').unlink();self.invoke('slots-retire',expected=1)
        self.assertFalse((self.store/'slots-retired').exists())

    def test_receipt_survives_successor_key_rotation(self):
        self.terminal_pair();self.invoke('slots-retire');receipt=(self.store/'slots-retired').read_bytes()
        (self.root/'aetherpd-upgrade').rmdir()
        for name in ['pd-boot','BOOT-LICENSES.txt']:
            self.fixture.write(self.store/name,self.fixture.files[1][name],0o755 if name=='pd-boot' else 0o644)
        for name in units.render('arm64'):
            for directory in [self.store/'units',self.root/'units']:
                self.fixture.write(directory/name,self.fixture.files[1][name],0o644)
        self.fixture.next_inputs(True);self.stage();self.begin();self.invoke('select');self.invoke('candidate');self.begin(4)
        self.invoke('slots-retire');self.assertEqual(receipt,(self.store/'slots-retired').read_bytes());self.invoke('active')

    def test_matching_partial_receipt_resumes_and_foreign_prefix_refuses(self):
        self.terminal_pair();self.invoke('slots-retire',expected=-signal.SIGKILL,fault='slots-durable:0')
        path=self.store/'slots-retired';data=path.read_bytes();path.unlink()
        for length in [0,8,360,408,500,620,651,652]:
            with self.subTest(length=length):
                self.fixture.write(self.store/'slots-retired.new',data[:length])
                self.invoke('slots-retire',expected=-signal.SIGKILL,fault='slots-durable:0')
                self.assertEqual(path.read_bytes(),data);path.unlink()
        self.fixture.write(self.store/'slots-retired.new',b'foreign');before=self.snapshot(self.store)
        self.invoke('slots-retire',expected=1);self.assertEqual(before,self.snapshot(self.store))

    def test_slot_aliases_and_wrong_modes_refuse(self):
        self.terminal_pair();path=self.store/'slots/A.bundle';alias=self.root/'alias'
        path.chmod(0o644);self.invoke('slots-retire',expected=1);path.chmod(0o600)
        os.link(path,alias);self.invoke('slots-retire',expected=1);alias.unlink()
        path.rename(alias);path.symlink_to(alias);before=alias.read_bytes();self.invoke('slots-retire',expected=1)
        self.assertEqual(alias.read_bytes(),before)

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--checker',required=True,type=Path);p.add_argument('--upgrade-checker',required=True,type=Path)
    a,rest=p.parse_known_args();test_generation.CHECKER=a.checker.resolve();test_upgrade.CHECKER=a.upgrade_checker.resolve()
    unittest.main(argv=[__file__,*rest])
