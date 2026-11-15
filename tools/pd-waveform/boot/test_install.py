#!/usr/bin/env python3
"""Persistent preparation tests using protected disposable host directories."""
import argparse
import hashlib
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
import test_release
import bundle
import approval

CHECKER=None


class InstallTest(test_release.ReleaseTest):
    # Reuse fixture construction, not the release-verifier test methods.
    def setUp(self):
        super().setUp()
        self.parent=self.root/'parent';self.parent.mkdir(mode=0o700)
        self.units=self.root/'systemd';self.units.mkdir(mode=0o700)
        self.stage=self.root/'stage';self.stage.mkdir(mode=0o700)
        self.input=self.root/'release';self.input.write_bytes(self.good)
        self.approved=self.root/'approved';self.approved.write_bytes(bundle.blake(self.good))

    def invoke(self,extract=True,expected=0,fault=''):
        r=subprocess.run([str(CHECKER),str(self.input),str(self.key),str(self.approved),str(self.parent),
            str(self.units),str(self.stage),'extract' if extract else 'prepared',fault],capture_output=True,timeout=10)
        self.assertEqual(r.returncode,expected,r.stderr.decode(errors='replace'))

    def verify(self):
        store=self.parent/'aetherpd'
        self.assertEqual((store/'pd-boot').read_bytes(),self.files['pd-boot'])
        self.assertEqual(store.stat().st_mode&0o7777,0o755)
        self.assertEqual((store/'state').stat().st_mode&0o7777,0o700)
        self.assertFalse((store/'pd-update').exists())
        for name in list(self.files)[4:]:
            self.assertEqual((store/'units'/name).read_bytes(),self.files[name])
            self.assertFalse((self.units/name).is_symlink())
            self.assertEqual((self.units/name).read_bytes(),self.files[name])
            self.assertEqual((self.units/name).stat().st_mode&0o7777,0o644)
        self.assertFalse((self.units/'multi-user.target.wants').exists())
        self.assertEqual(len(list((store/'slots').glob('*.bundle'))),1)

    def stage_for_discard(self):
        self.invoke();self.stage.rename(self.root/'aetherpd-install');self.stage=self.root/'aetherpd-install'
        self.target=self.root/'target';self.target.write_bytes(approval.encode(1,'4.3.16.42034','fixture',bundle.blake(self.good)))
        (self.stage/'approved').write_bytes(self.target.read_bytes());(self.stage/'approved').chmod(0o600)

    def discard(self,expected=0):
        result=subprocess.run([str(CHECKER),str(self.input),str(self.key),str(self.approved),str(self.root),str(self.units),str(self.stage),'discard',str(self.target)],capture_output=True,timeout=10)
        self.assertEqual(result.returncode,expected,result.stderr.decode(errors='replace'))
        self.verify()

    def test_stage_discard_preserves_installed_files_and_repeats(self):
        self.stage_for_discard();self.discard();self.assertFalse(self.stage.exists());self.discard()

    def test_stage_discard_preserves_unknown_and_tampered_files(self):
        self.stage_for_discard();unknown=self.stage/'foreign';unknown.write_bytes(b'foreign')
        before={p.name:p.read_bytes() for p in self.stage.iterdir()};self.discard(1)
        self.assertEqual(before,{p.name:p.read_bytes() for p in self.stage.iterdir()});unknown.unlink()
        helper=self.stage/'pd-boot';helper.write_bytes(b'corrupt');self.discard(1);self.assertTrue((self.stage/'payload.bundle').exists())

    def test_stage_discard_resumes_missing_files(self):
        self.stage_for_discard();(self.stage/'pd-boot').unlink();(self.stage/'approved').unlink();self.discard();self.assertFalse(self.stage.exists())

    def test_stage_discard_preserves_foreign_approval(self):
        self.stage_for_discard();(self.stage/'approved').write_bytes(b'foreign');self.discard(1)
        self.assertTrue((self.stage/'pd-boot').exists())

    def test_prepare_and_idempotency(self):
        self.invoke();self.verify();inode=(self.parent/'aetherpd/pd-boot').stat().st_ino
        self.invoke(extract=False);self.verify();self.assertEqual((self.parent/'aetherpd/pd-boot').stat().st_ino,inode)

    def test_interruption_resumes_without_enabling(self):
        for point in ['intent-written','fixed-files-written','payload-selected','store-synced','store-published','unit-published']:
            with self.subTest(point=point),tempfile.TemporaryDirectory(dir=self.root) as d:
                previous=self.parent,self.units,self.stage
                self.parent=Path(d)/'parent';self.units=Path(d)/'units';self.stage=Path(d)/'stage'
                for p in [self.parent,self.units,self.stage]:p.mkdir(mode=0o700)
                self.invoke(expected=88,fault=point)
                self.assertFalse((self.units/'multi-user.target.wants').exists())
                self.invoke(extract=False);self.verify()
                self.parent,self.units,self.stage=previous

    def test_foreign_store_not_adopted(self):
        p=self.parent/'aetherpd';p.mkdir();(p/'foreign').write_bytes(b'keep')
        self.invoke(expected=1);self.assertEqual(list(x.name for x in p.iterdir()),['foreign'])

    def test_foreign_preparation_directory_not_adopted(self):
        p=self.parent/'.aetherpd-prepare';p.mkdir();(p/'foreign').write_bytes(b'keep')
        self.invoke(expected=1);self.assertEqual(list(x.name for x in p.iterdir()),['foreign'])

    def test_foreign_unit_not_overwritten(self):
        p=self.units/'aetherpd-boot.service';p.write_bytes(b'keep')
        self.invoke(expected=1);self.assertEqual(p.read_bytes(),b'keep')
        self.assertFalse((self.units/'multi-user.target.wants').exists())

    def test_changed_staged_helper_refused(self):
        self.invoke();(self.stage/'pd-boot').write_bytes(b'corrupt')
        original=(self.parent/'aetherpd/pd-boot').read_bytes()
        self.invoke(extract=False,expected=1)
        self.assertEqual((self.parent/'aetherpd/pd-boot').read_bytes(),original)

    def test_store_symlink_refused(self):
        foreign=self.root/'foreign';foreign.mkdir();(self.parent/'aetherpd').symlink_to(foreign)
        self.invoke(expected=1);self.assertEqual(list(foreign.iterdir()),[])
        self.assertFalse((self.parent/'.aetherpd-prepare').exists())

    def test_writable_foreign_store_refused(self):
        p=self.parent/'aetherpd';p.mkdir();p.chmod(0o777)
        self.invoke(expected=1)
        self.assertEqual(list(p.iterdir()),[])
        self.assertFalse((self.parent/'.aetherpd-prepare').exists())

    def test_dangling_enable_link_refused(self):
        p=self.units/'multi-user.target.wants';p.mkdir()
        (p/'aetherpd-shim.service').symlink_to('../aetherpd-shim.service')
        self.invoke(expected=1)
        self.assertFalse((self.parent/'aetherpd').exists())
        self.assertTrue((p/'aetherpd-shim.service').is_symlink())

    def test_partial_copy_after_mode_change_recovers(self):
        self.invoke(expected=88,fault='intent-written')
        p=self.parent/'.aetherpd-prepare/pd-boot.new';p.write_bytes(self.files['pd-boot']);p.chmod(0o755)
        self.invoke(extract=False);self.verify()

    def test_units_readable_before_payload_mount(self):
        self.invoke()
        (self.parent/'aetherpd').rename(self.parent/'unmounted')
        for name in list(self.files)[4:]:
            self.assertEqual((self.units/name).read_bytes(),self.files[name])

    def test_changed_installed_unit_refused(self):
        self.invoke()
        unit=self.units/'aetherpd-boot.service';unit.write_bytes(b'foreign replacement')
        self.invoke(extract=False,expected=1)
        self.assertEqual(unit.read_bytes(),b'foreign replacement')

    def test_installed_unit_symlink_refused(self):
        self.invoke()
        unit=self.units/'aetherpd-boot.service';unit.unlink()
        unit.symlink_to(self.parent/'aetherpd/units/aetherpd-boot.service')
        self.invoke(extract=False,expected=1)
        self.assertTrue(unit.is_symlink())


# Base class supplies only fixtures; do not run its separate verifier suite here.
for name in list(vars(test_release.ReleaseTest)):
    if name.startswith('test_') and name not in vars(InstallTest):setattr(InstallTest,name,None)

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--checker',type=Path,required=True)
    args,rest=parser.parse_known_args();CHECKER=args.checker.resolve()
    unittest.main(argv=['test_install.py',*rest])
