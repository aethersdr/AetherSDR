#!/usr/bin/env python3
"""Execute the fixed upgrade worker in a disposable user/network namespace.

The fixture supplies already authenticated bootstrap outputs. Real signatures,
leases, staged releases, management replacements, selectors, journals and
cleanup execute; systemd, radio identity and process attestation are adapters.
"""
import argparse
import fcntl
import os
from pathlib import Path
import shutil
import struct
import subprocess
import unittest
import bundle
import release
import test_management
import units
import upgrade_units
CHECKER=None

class UpgradeWorkerTest(unittest.TestCase):
    def setUp(self):
        self.fixture=test_management.ManagementTest(methodName='runTest');self.fixture.setUp()
        self.addCleanup(self.fixture.doCleanups);self.root=self.fixture.root
        for path in ['var/lib/smartsdr','etc/systemd/system','run','proc','dev','fixture']:
            (self.root/path).mkdir(parents=True,exist_ok=True,mode=0o755)
        self.store=self.root/'var/lib/smartsdr/aetherpd';self.fixture.store.rename(self.store)
        self.units=self.root/'etc/systemd/system';self.units.rmdir();self.fixture.units.rename(self.units)
        self.capsule=self.store.parent/'aetherpd-upgrade';(self.root/'aetherpd-upgrade').rename(self.capsule)
        self.put(self.capsule/'worker',CHECKER.read_bytes(),0o700)
        self.put(self.root/'fixture/copy',CHECKER.read_bytes(),0o700)
        self.put(self.root/'fixture/entry',(CHECKER.parent/'upgrade-worker-namespace').read_bytes(),0o700)
        self.put(self.root/'fixture/launched',b'')
        public=b'fixture'.ljust(64,b'\0')+b'100.64.1.2'.ljust(16,b'\0')+b'fixture.example.ts.net'.ljust(256,b'\0')
        self.put(self.capsule/'intent',b'PDUPCAP1'+self.fixture.approved+bundle.blake(CHECKER.read_bytes())+public)
        self.stage=self.store/'upgrade-pair';self.stage.mkdir(mode=0o700)
        self.control=self.store/'upgrade-control';self.control.mkdir(mode=0o700)
        self.put(self.stage/'approved',self.fixture.approved)
        for i,name in enumerate(['previous','candidate']):
            directory=self.stage/name;directory.mkdir(mode=0o700)
            self.put(directory/'release.header',self.fixture.artifacts[i][:release.PREFIX])
            for leaf,data in self.fixture.files[i].items():self.put(directory/leaf,data,0o755 if leaf in ['pd-boot','pd-update'] else 0o644)
        self.put(self.control/'lock',b'')
        self.put(self.control/'active',b'PDPAIR01'+bytes(8)+bundle.blake(self.fixture.approved)+bundle.blake(self.fixture.artifacts[0]))
        (self.store/'upgrade-transaction').unlink()
        for name,text in upgrade_units.render('arm64').items():self.put(self.units/name,text.encode(),0o644)
        wants=self.units/'multi-user.target.wants';wants.mkdir(mode=0o755)
        (wants/'aetherpd-upgrade-recover.service').symlink_to('../aetherpd-upgrade-recover.service')
        self.put(self.root/'fixture/running',b'1');self.put(self.root/'fixture/fault',b'')
        self.put(self.store/'installed',b'PDINST01'+struct.pack('>II',64,1)+self.fixture.keys[0])
        (self.store/'slots').mkdir(mode=0o700)
        payload=self.fixture.files[0]['payload.bundle'];self.put(self.store/'slots/A.bundle',payload)
        self.put(self.store/'active',b'PDSEL002A'+bytes(7)+bundle.blake(payload)+bytes(40))
        runtime=self.root/'run/aetherpd/bin';runtime.mkdir(parents=True,mode=0o755)
        for name in bundle.NAMES:
            data=self.fixture.files[0]['pd-boot'] if name!='LICENSES' else b'fixture'
            self.put(runtime/name,data,0o644 if name=='LICENSES' else 0o755)
        for tail in ['primary','primary/tsnet','primary/tsnet/profile-data','primary/tsnet/profile-data/8fd1','primary/tsnet/profile-data/8fd1/netmap-cache']:
            (self.root/'run/aetherpd'/tail).mkdir(mode=0o700)
        self.put(self.root/'run/aetherpd/primary/tsnet/profile-data/8fd1/netmap-cache/73656c66',b'volatile cache')
    def put(self,path,data,mode=0o600):path.write_bytes(data);path.chmod(mode)
    def events(self):
        p=self.root/'fixture/events';return p.read_text().splitlines() if p.exists() else []
    def invoke(self,recovery=False,fault='',expected=0,copy=False,bootstrap=False):
        (self.root/'fixture/fault').write_text(fault)
        executable='/fixture/entry' if bootstrap else '/fixture/copy' if copy else '/var/lib/smartsdr/aetherpd-upgrade/worker'
        mounts=['--ro-bind',str(self.persistent_inputs),'/persistent-inputs'] if hasattr(self,'persistent_inputs') else []
        command=['bwrap','--unshare-user','--uid','0','--gid','0','--unshare-pid','--unshare-net','--unshare-ipc','--unshare-uts',
                 '--bind',str(self.root),'/','--proc','/proc','--dev','/dev','--tmpfs','/release-inputs',*mounts,
                 '--die-with-parent','--new-session','--clearenv','--chdir','/',executable]
        if recovery:command.append('--recover')
        r=subprocess.run(command,capture_output=True,timeout=20)
        self.assertEqual(r.returncode,expected,r.stderr.decode(errors='replace')+'\nevents='+str(self.events()))
    def finished(self,choice):
        self.assertEqual((self.control/'active').read_bytes()[8],choice)
        self.assertEqual((self.store/'upgrade-transaction').read_bytes(),b'PDUTXN01'+self.fixture.approved+struct.pack('>I',4 if choice else 7))
        self.assertEqual((self.root/'fixture/running').read_bytes(),str(choice+1).encode())
        self.assertEqual((self.store/'state/private-fixture').read_bytes(),b'preserve identity')
        self.assertFalse(self.capsule.exists());self.assertFalse((self.store.parent/'.aetherpd-upgrade-retired').exists())
        for name in upgrade_units.render('arm64'):self.assertFalse((self.units/name).exists())
        self.assertFalse((self.units/'multi-user.target.wants/aetherpd-upgrade-recover.service').is_symlink())
        for directory,names in [(self.store,['pd-boot','BOOT-LICENSES.txt']),
                                (self.store/'units',units.render('arm64')),(self.units,units.render('arm64'))]:
            for name in names:self.assertEqual((directory/name).read_bytes(),self.fixture.files[1][name])
        self.assertEqual((self.root/'run/aetherpd/bin/flex-tailnet-shim').read_bytes(),self.fixture.files[choice]['pd-boot'])
    def test_forward_commit_and_authority_removal(self):
        self.invoke();self.finished(1)
        events=self.events();self.assertLess(events.index('quiesce:after'),events.index('transaction:upgrade-file:1'))
    def retained_management_origin(self):
        self.fixture.targets=[(self.store/p.relative_to(self.fixture.store) if p.is_relative_to(self.fixture.store)
                               else self.units/p.relative_to(self.fixture.units),name)
                              for p,name in self.fixture.targets]
        origin=self.fixture.retained_management()
        intent=(self.capsule/'intent').read_bytes()
        self.put(self.capsule/'intent',b'PDUPCAP2'+intent[8:728]+origin)
    def test_forward_with_retained_management_from_other_key(self):
        self.retained_management_origin();self.invoke();self.finished(1)
    def test_rollback_with_retained_management_from_other_key(self):
        self.retained_management_origin();self.invoke(recovery=True);self.finished(0)
    def test_interrupted_origin_management_recovers_without_candidate(self):
        self.retained_management_origin();self.invoke(fault='kill:management:rename:0',expected=137)
        (self.stage/'candidate/payload.bundle').write_bytes(b'rejected payload damaged')
        self.invoke(recovery=True);self.finished(0)
    def test_changed_origin_refuses_before_quiescence(self):
        self.retained_management_origin();p=self.capsule/'intent';raw=bytearray(p.read_bytes());raw[-1]^=1
        self.put(p,raw);self.invoke(expected=3);self.assertNotIn('quiesce:before',self.events())
    def test_origin_capsule_requires_permanent_cleanup_support(self):
        self.retained_management_origin()
        if self.fixture.artifacts[1][:8] in (b'AEPDR004',b'AEPDR005'):
            self.invoke();self.finished(1)
        else:
            self.invoke(expected=3);self.assertNotIn('quiesce:before',self.events())
    def test_recovery_before_journal_only_restores(self):
        self.invoke(recovery=True);self.finished(0);self.assertNotIn('candidate:activate-before',self.events())
    def test_reject_copy_before_quiescence(self):
        self.invoke(copy=True,expected=3);self.assertNotIn('quiesce:before',self.events())
    def test_reject_wrong_worker_metadata(self):
        (self.capsule/'worker').chmod(0o755);self.invoke(expected=3);self.assertFalse(self.events())
    def test_reject_bad_previous_before_quiescence(self):
        p=self.stage/'previous/payload.bundle';p.write_bytes(b'bad');self.invoke(expected=3)
        self.assertNotIn('quiesce:before',self.events());self.assertEqual((self.root/'fixture/running').read_bytes(),b'1')
    def test_original_installation_must_be_committed_before_quiescence(self):
        p=self.store/'transaction';data=p.read_bytes();self.put(p,data[:-1]+b'\x04')
        self.invoke(expected=3);self.assertNotIn('quiesce:before',self.events())
    def test_foreign_partial_journal_refuses_before_quiescence(self):
        self.put(self.store/'upgrade-transaction.new',b'foreign');self.invoke(expected=3)
        self.assertNotIn('quiesce:before',self.events())
        self.assertEqual((self.store/'upgrade-transaction.new').read_bytes(),b'foreign')
    def test_matching_partial_journal_restores_previous(self):
        self.put(self.store/'upgrade-transaction.new',(b'PDUTXN01'+self.fixture.approved)[:97])
        self.invoke();self.finished(0)
    def test_bad_candidate_restores_previous(self):
        (self.stage/'candidate/payload.bundle').write_bytes(b'bad');self.invoke();self.finished(0)
    def test_target_and_service_admission_fail_before_quiescence(self):
        self.invoke(fault='fail:identity',expected=4)
        self.invoke(fault='fail:service:worker-admission',expected=3)
        self.assertNotIn('quiesce:before',self.events())
    def test_exclusive_namespace_owner_prevents_other_worker(self):
        p=self.store.parent/'.aetherpd-upgrade.lock';self.put(p,b'')
        with p.open('r+b') as fd:
            fcntl.flock(fd,fcntl.LOCK_EX);self.invoke(expected=4)
        self.assertFalse(self.events())
    def test_refused_quiescence_leaves_previous_running(self):
        self.invoke(fault='fail:quiesce:before',expected=4)
        self.assertEqual((self.root/'fixture/running').read_bytes(),b'1');self.assertFalse((self.store/'upgrade-transaction').exists())
    def test_returned_activation_failure_restores(self):
        self.invoke(fault='fail:candidate:activate-after');self.finished(0)
    def test_crash_during_management_repair_then_damaged_candidate_recovers(self):
        self.invoke(fault='kill:management:rename:8',expected=137)
        (self.stage/'candidate/payload.bundle').write_bytes(b'rejected damaged payload')
        self.invoke(recovery=True);self.finished(0)
    def test_crash_before_transaction_recovers(self):
        self.invoke(fault='kill:quiesce:after',expected=137);self.invoke(recovery=True);self.finished(0)
    def test_automatic_forward_restart_after_prejournal_crash_rolls_back(self):
        self.invoke(fault='kill:quiesce:after',expected=137);self.invoke();self.finished(0)
    def test_partial_started_prefix_forces_rollback(self):
        started=b'PDUPRUN1'+bundle.blake((self.capsule/'intent').read_bytes())
        self.put(self.capsule/'started',started[:13]);self.invoke();self.finished(0)
    def test_foreign_started_receipt_refuses_before_service_stop(self):
        self.put(self.capsule/'started',b'foreign');self.invoke(expected=3)
        self.assertNotIn('quiesce:before',self.events())
    def test_crash_after_selection_recovers(self):
        self.invoke(fault='kill:pair:pair-committed:1',expected=137);self.invoke(recovery=True);self.finished(0)
    def test_crash_after_activation_recovers(self):
        self.invoke(fault='kill:candidate:activate-after',expected=137);self.invoke(recovery=True);self.finished(0)
    def test_terminal_retry_only_cleans(self):
        self.invoke(fault='kill:transaction:upgrade-durable:4',expected=137)
        before=self.events();self.invoke(recovery=True);self.finished(1)
        self.assertEqual(self.events().count('quiesce:before'),before.count('quiesce:before'))
        self.assertEqual(self.events().count('candidate:activate-before'),1)
    def test_recovery_interrupted_again_resumes(self):
        self.invoke(fault='kill:candidate:activate-after',expected=137)
        self.invoke(recovery=True,fault='kill:runtime:unlink',expected=137)
        self.invoke(recovery=True);self.finished(0)
    def unprepare(self):
        for path in [self.capsule,self.stage,self.control]:shutil.rmtree(path)
        for name in upgrade_units.render('arm64'):(self.units/name).unlink()
        (self.units/'multi-user.target.wants/aetherpd-upgrade-recover.service').unlink()
        (self.root/'fixture/launched').unlink()
        self.put(self.root/'fixture/approved',self.fixture.approved)
        for i,name in enumerate(['previous','candidate']):self.put(self.root/('fixture/'+name+'.release'),self.fixture.artifacts[i])
    def test_bootstrap_arms_recovery_without_stopping_runtime(self):
        self.unprepare();self.invoke(bootstrap=True)
        self.assertEqual((self.root/'fixture/running').read_bytes(),b'1')
        self.assertNotIn('quiesce:before',self.events());self.assertTrue(self.capsule.is_dir())
        intent=(self.capsule/'intent').read_bytes()
        if self.fixture.artifacts[1][:8] in (b'AEPDR004',b'AEPDR005'):
            self.assertEqual(intent[:8],b'PDUPCAP2')
            self.assertEqual(intent[728:],b'PDORIG01'+bundle.blake(self.fixture.approved)+bytes(364)+self.fixture.artifacts[0][:release.PREFIX])
        else:self.assertEqual(intent[:8],b'PDUPCAP1');self.assertEqual(len(intent),728)
        self.assertFalse((self.capsule/'started').exists())
        self.assertTrue((self.units/'multi-user.target.wants/aetherpd-upgrade-recover.service').is_symlink())
        self.invoke();self.finished(1)
    def test_bootstrap_space_refusal_before_staging_files(self):
        self.unprepare();self.invoke(bootstrap=True,fault='fail:space:bytes',expected=4)
        self.assertFalse(self.capsule.exists());self.assertFalse(list(self.stage.iterdir()))
        self.assertEqual((self.root/'fixture/running').read_bytes(),b'1')
    def test_bootstrap_reserves_management_and_capsule_beyond_pair_staging(self):
        self.unprepare();block=os.statvfs(self.store).f_frsize
        sizes=[len(self.fixture.approved),release.PREFIX,release.PREFIX]
        sizes.extend(len(data) for files in self.fixture.files for data in files.values())
        pair_space=8*1024*1024+3*block+sum(((size+block-1)//block)*block for size in sizes)
        self.put(self.root/'fixture/free-bytes',str(pair_space+65536).encode())
        self.invoke(bootstrap=True,expected=4);self.assertFalse(list(self.stage.iterdir()))
        self.assertFalse(self.capsule.exists());self.assertNotIn('quiesce:before',self.events())
    def test_bootstrap_reserves_future_inodes(self):
        self.unprepare();self.put(self.root/'fixture/free-inodes',b'25')
        self.invoke(bootstrap=True,expected=4);self.assertFalse(list(self.stage.iterdir()))
        self.assertFalse(self.capsule.exists())
    def test_bootstrap_wrong_embedded_candidate_binding_refuses(self):
        self.unprepare();p=self.root/'fixture/approved';data=bytearray(p.read_bytes());data[-65]^=1;self.put(p,data)
        self.invoke(bootstrap=True,expected=3);self.assertFalse(self.stage.exists());self.assertFalse(self.events())
    def test_bootstrap_rejects_changed_running_payload(self):
        self.unprepare();p=self.root/'run/aetherpd/bin/flex-tailnet-shim';p.write_bytes(b'foreign')
        self.invoke(bootstrap=True,expected=4);self.assertFalse(self.capsule.exists())
        self.assertNotIn('quiesce:before',self.events());self.assertEqual(p.read_bytes(),b'foreign')
    def test_bootstrap_retries_partial_stage(self):
        self.unprepare();self.invoke(bootstrap=True,fault='kill:pair:resume-copy:5',expected=137)
        self.invoke(bootstrap=True);self.invoke();self.finished(1)
    def test_bootstrap_rearms_published_capsule_without_input_releases(self):
        self.unprepare();self.invoke(bootstrap=True,fault='kill:upgrade-capsule-rename',expected=137)
        for name in ['previous','candidate']:(self.root/('fixture/'+name+'.release')).write_bytes(b'unavailable')
        self.invoke(bootstrap=True);self.invoke();self.finished(1)
    def test_bootstrap_rearm_after_prejournal_stop_goes_to_rollback(self):
        self.unprepare();self.invoke(bootstrap=True);self.invoke(fault='kill:quiesce:after',expected=137)
        self.invoke(bootstrap=True);self.invoke();self.finished(0)
    def test_bootstrap_rearm_during_mixed_management_replacement(self):
        self.unprepare();self.invoke(bootstrap=True);self.invoke(fault='kill:management:rename:8',expected=137)
        self.invoke(bootstrap=True);self.invoke();self.finished(0)
    def test_bootstrap_terminal_detached_capsule_cleanup(self):
        self.unprepare();self.invoke(bootstrap=True);self.invoke(fault='kill:upgrade-retire-rename',expected=137)
        self.invoke(bootstrap=True);self.finished(1)
    def test_bootstrap_exclusive_lease_refuses_before_staging(self):
        self.unprepare();p=self.store.parent/'.aetherpd-upgrade-bootstrap.lock';self.put(p,b'')
        with p.open('r+b') as fd:
            fcntl.flock(fd,fcntl.LOCK_EX);self.invoke(bootstrap=True,expected=4)
        self.assertFalse(self.stage.exists());self.assertFalse(self.events())

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--checker',type=Path,required=True)
    a,rest=p.parse_known_args();CHECKER=a.checker.resolve();unittest.main(argv=[__file__,*rest])
