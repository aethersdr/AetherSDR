#!/usr/bin/env python3
"""Fixed identity transfer, refusal and interruption recovery; no network."""
import argparse
import fcntl
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
import approval

CHECKER=None
NATIVE_OWNERS=False
RESTORE=False
SERIAL='2125-1213-8600-8895'


class StateTransferTest(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(prefix='pd-state-');self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name)
        self.user=(0 if RESTORE else 78666) if NATIVE_OWNERS else os.getuid()
        self.group=(0 if RESTORE else 20) if NATIVE_OWNERS else os.getgid()
        self.source_user=(78666 if RESTORE else 0) if NATIVE_OWNERS else os.getuid()
        self.source_group=(20 if RESTORE else 0) if NATIVE_OWNERS else os.getgid()
        self.config=b'{"hostname":"fixture","token_sha256":"fixture-not-a-credential"}\n'
        self.identity=b'{"fixture":"not-a-real-node-identity"}\n'
        self.paths(self.root/'case')

    def paths(self,root):
        root.mkdir();self.store=root/'store';self.store.mkdir(mode=0o755)
        self.state=self.store/'state';self.state.mkdir(mode=0o700);os.chown(self.state,self.user,self.group)
        self.record=root/'approval';self.record.write_bytes(approval.encode(1,'4.3.16.42034',SERIAL,bytes(range(32))))
        self.provision=root/'provision';self.tailscale=root/'tailscale'
        for path,data in [(self.provision,self.config),(self.tailscale,self.identity)]:
            path.write_bytes(data);path.chmod(0o600);os.chown(path,self.source_user,self.source_group)

    def invoke(self,expected=0,fault=''):
        args=[str(CHECKER),str(self.store),str(self.record),str(self.provision),str(self.tailscale)]
        if fault:args.append(fault)
        if RESTORE:args.append('--restore')
        result=subprocess.run(args,capture_output=True,timeout=10)
        self.assertEqual(result.returncode,expected,result.stderr.decode(errors='replace'))

    def verify(self):
        self.assertEqual((self.state/'provision.json').read_bytes(),self.config)
        self.assertEqual((self.state/'tsnet/tailscaled.state').read_bytes(),self.identity)
        for path in [self.state,self.state/'tsnet',self.state/'provision.json',self.state/'tsnet/tailscaled.state']:
            st=path.stat();self.assertEqual(st.st_uid,self.user);self.assertEqual(st.st_gid,self.group)
            self.assertEqual(st.st_mode&0o7777,0o700 if path.is_dir() else 0o600)
        self.assertFalse((self.store/'.identity-transfer').exists())
        self.assertFalse((self.store/'identity-transfer').exists())
        marker=self.store/'identity-transferred';self.assertEqual(marker.stat().st_mode&0o7777,0o600)
        self.assertEqual(marker.stat().st_uid,os.getuid())
        for path,data in [(self.provision,self.config),(self.tailscale,self.identity)]:
            if path.exists():self.assertEqual(path.read_bytes(),data)

    def test_transfer_and_idempotent_retry(self):
        self.invoke();self.verify()
        ino=(self.state/'provision.json').stat().st_ino
        self.provision.unlink();self.tailscale.unlink();self.invoke();self.verify()
        self.assertEqual((self.state/'provision.json').stat().st_ino,ino)

    def test_committed_identity_edits_not_reimported(self):
        self.invoke();changed=b'{"new":"operator identity"}'
        (self.state/'provision.json').write_bytes(changed)
        self.invoke();self.assertEqual((self.state/'provision.json').read_bytes(),changed)

    def test_interruptions_resume(self):
        points=['sources-captured','stage-created','intent-recorded','config-copied','identity-copied',
                'snapshot-prepared','state-published','completion-recorded','config-owned','identity-owned','nested-owned','state-released']
        for i,point in enumerate(points):
            with self.subTest(point=point):
                self.paths(self.root/point);self.invoke(expected=88,fault=point)
                if NATIVE_OWNERS and 6<=i<11:
                    self.assertEqual(self.state.stat().st_uid,0)
                if i>=5:self.provision.unlink();self.tailscale.unlink()
                self.invoke();self.verify()

    def test_existing_identity_not_overwritten(self):
        (self.state/'foreign').write_bytes(b'keep')
        self.invoke(expected=1);self.assertEqual((self.state/'foreign').read_bytes(),b'keep')
        self.assertFalse((self.store/'.identity-transfer').exists())

    def test_source_metadata_and_size(self):
        for name,data,mode in [('empty',b'',0o600),('oversized',b'x'*65537,0o600),('readable',self.config,0o644)]:
            with self.subTest(name=name):
                self.paths(self.root/name);self.provision.write_bytes(data);self.provision.chmod(mode)
                self.invoke(expected=1);self.assertFalse((self.store/'.identity-transfer').exists())

    def test_source_links_and_fifo(self):
        os.link(self.provision,self.root/'alias');self.invoke(expected=1);(self.root/'alias').unlink()
        self.provision.unlink();self.provision.symlink_to(self.tailscale);self.invoke(expected=1)
        self.provision.unlink();os.mkfifo(self.provision,0o600);self.invoke(expected=1)
        self.assertEqual(list(self.state.iterdir()),[])

    def test_source_change_during_capture_refused(self):
        self.invoke(expected=1,fault='mutate-source')
        self.assertFalse((self.store/'.identity-transfer').exists());self.assertEqual(list(self.state.iterdir()),[])

    def test_foreign_stage_and_symlink_not_adopted(self):
        stage=self.store/'.identity-transfer';stage.mkdir(mode=0o700);(stage/'foreign').write_bytes(b'keep')
        self.invoke(expected=1);self.assertEqual((stage/'foreign').read_bytes(),b'keep')
        (stage/'foreign').unlink();stage.rmdir();stage.symlink_to(self.root)
        self.invoke(expected=1);self.assertFalse((self.root/'intent').exists())

    def test_target_symlink_refused(self):
        self.state.rmdir();self.state.symlink_to(self.root)
        self.invoke(expected=1);self.assertFalse((self.root/'provision.json').exists())

    def test_other_target_approval_cannot_resume(self):
        self.invoke(expected=88,fault='snapshot-prepared')
        self.record.write_bytes(approval.encode(1,'4.3.16.42034','another-radio',bytes(range(32))))
        self.invoke(expected=1);self.assertEqual(list(self.state.iterdir()),[])

    def test_prepared_snapshot_damage_refused(self):
        self.invoke(expected=88,fault='snapshot-prepared')
        (self.store/'.identity-transfer/provision.json').write_bytes(b'!'+self.config[1:])
        self.invoke(expected=1);self.assertEqual(list(self.state.iterdir()),[])

    def test_partial_copy_recovered_only_with_matching_prefix(self):
        self.invoke(expected=88,fault='intent-recorded')
        p=self.store/'.identity-transfer/provision.json.new';p.write_bytes(self.config[:8]);p.chmod(0o600)
        self.invoke();self.verify()
        self.paths(self.root/'foreign-partial');self.invoke(expected=88,fault='intent-recorded')
        p=self.store/'.identity-transfer/provision.json.new';p.write_bytes(b'foreign');p.chmod(0o600)
        self.invoke(expected=1);self.assertEqual(p.read_bytes(),b'foreign')

    def test_completed_marker_damage_refused(self):
        self.invoke();marker=self.store/'identity-transferred';marker.write_bytes(b'foreign')
        self.invoke(expected=1);self.assertEqual(marker.read_bytes(),b'foreign')
        self.assertEqual((self.state/'provision.json').read_bytes(),self.config)

    def test_store_lock_and_permissions(self):
        fd=os.open(self.store/'store.lock',os.O_RDWR|os.O_CREAT,0o600)
        try:
            fcntl.flock(fd,fcntl.LOCK_EX|fcntl.LOCK_NB);self.invoke(expected=1)
        finally:os.close(fd)
        self.store.chmod(0o777);self.invoke(expected=1)
        self.assertEqual(list(self.state.iterdir()),[])

    def test_native_source_owner(self):
        if not NATIVE_OWNERS:self.skipTest('distinct root/waveform UIDs require native root fixture')
        os.chown(self.provision,0 if RESTORE else 78666,0 if RESTORE else 20);self.invoke(expected=1)
        self.assertFalse((self.store/'.identity-transfer').exists())

    def test_waveform_profile_identity(self):
        self.record.write_bytes(approval.encode(2,'4.2.18.41174','fixture-6300',bytes(range(32))))
        if RESTORE:
            self.invoke(expected=1);self.assertEqual(list(self.state.iterdir()),[]);return
        for p in [self.provision,self.tailscale]:os.chown(p,self.user,self.group)
        self.invoke();self.verify()

    def test_direction_specific_journal(self):
        self.invoke();marker=self.store/'identity-transferred'
        self.assertEqual(marker.read_bytes()[:8],b'PDROLL01' if RESTORE else b'PDSTATE1')
        args=[str(CHECKER),str(self.store),str(self.record),str(self.provision),str(self.tailscale)]
        if not RESTORE:args.append('--restore')
        result=subprocess.run(args,capture_output=True,timeout=10);self.assertEqual(result.returncode,1)
        self.verify()


if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--checker',type=Path,required=True)
    parser.add_argument('--native-owners',action='store_true')
    parser.add_argument('--restore',action='store_true')
    args,rest=parser.parse_known_args();CHECKER=args.checker.resolve();NATIVE_OWNERS=args.native_owners;RESTORE=args.restore
    if NATIVE_OWNERS and os.geteuid()!=0:parser.error('native owner fixture requires root')
    unittest.main(argv=['test_state_transfer.py',*rest])
