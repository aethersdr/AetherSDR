#!/usr/bin/env python3
"""Fixed bootstrap staging, descriptor, consent and lifetime component tests.

The checker substitutes only live identity observation and exec of inert ARM
fixture bytes. It runs as the test user in temporary directories, never on radio.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import signal
import select
import struct
import subprocess
import tempfile
import unittest
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
import approval
import bootstrap_artifact
import bundle
import delivery_consent
import migration
import first_install
import release
import units
import upgrade

CHECKER=None


class DeliveryTest(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(prefix='pd-delivery-');self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name);self.run=self.root/'run';self.run.mkdir(mode=0o700)
        self.seed=hashlib.sha256(b'delivery fixture candidate key').digest()
        self.old_seed=hashlib.sha256(b'delivery fixture predecessor key').digest()
        self.key=Ed25519PrivateKey.from_private_bytes(self.seed).public_key().public_bytes_raw()
        self.old_key=Ed25519PrivateKey.from_private_bytes(self.old_seed).public_key().public_bytes_raw()
        self.worker=bytearray(1024);self.worker[:7]=b'\x7fELF\x02\x01\x01';struct.pack_into('<HH',self.worker,16,2,183)
        self.source=hashlib.sha256(b'delivery fixture source').digest()
        files={n:bytes(self.worker) if n!='LICENSES' else b'fixture license' for n in bundle.NAMES}
        def package(seed):
            payload,_=bundle.assemble(files,64,self.source,seed)
            return release.assemble({'pd-boot':bytes(self.worker),'pd-update':bytes(self.worker),
                'payload.bundle':payload,'BOOT-LICENSES.txt':b'fixture license',
                **{n:v.encode() for n,v in units.render('arm64').items()}},'flex8600','4.3.16.42034',
                self.source,seed,paired_boot=False)[0]
        self.candidate=package(self.seed);self.previous=package(self.old_seed)
        self.plan=upgrade.encode(1,'4.3.16.42034','fixture-8600',bundle.blake(self.previous),bundle.blake(self.candidate),self.old_key,self.key)
        self.envelope=bootstrap_artifact.assemble(bytes(self.worker),64,2,self.source,bundle.blake(self.candidate),self.seed)
        self.prepare()

    def prepare(self):
        self.consent=delivery_consent.encode(self.envelope,self.plan)
        for name,data in [('key',self.key),('artifact',self.envelope),('consent',self.consent),('plan',self.plan),('candidate',self.candidate),('previous',self.previous)]:
            path=self.root/name;path.write_bytes(data);path.chmod(0o600)

    def command(self):
        return [str(CHECKER),str(self.run),*[str(self.root/n) for n in ['key','artifact','consent','plan','candidate','previous']],'64']

    def check(self,expected=0,mode='',clean=True):
        p=subprocess.run(self.command(),capture_output=True,text=True,timeout=10,
            env=dict(os.environ,PD_DELIVERY_TEST_MODE=mode,UNTRUSTED_DELIVERY_VARIABLE='must not be passed'))
        if mode=='worker-failure':
            self.assertEqual(json.loads(p.stderr),{'delivery_worker_exit':7,'delivery_worker_signal':0})
        else:self.assertEqual(p.stderr,'')
        self.assertEqual(p.returncode,expected,p.stdout)
        rows=[json.loads(line) for line in p.stdout.splitlines()]
        self.assertEqual(rows[-1],{'delivery_result':expected})
        if expected==0:self.assertIn({'verified_worker_interface':True},rows)
        if clean:self.assertEqual(list(self.run.iterdir()),[])
        return rows

    def test_upgrade_executes_only_verified_bytes_and_fixed_descriptor_interface(self):
        self.check()

    def test_first_install_executes_without_predecessor(self):
        self.plan=first_install.encode(approval.encode(1,'4.3.16.42034','fixture-8600',bundle.blake(self.candidate)),
                                      b't'*32,'fresh-radio',b'n'*32)
        self.envelope=bootstrap_artifact.assemble(bytes(self.worker),64,1,self.source,bundle.blake(self.candidate),self.seed)
        self.prepare()
        self.check()

    def test_first_install_rejects_noncanonical_identity_with_rebound_receipt(self):
        initial=first_install.encode(approval.encode(1,'4.3.16.42034','fixture-8600',bundle.blake(self.candidate)),
                                    b't'*32,'fresh-radio',b'n'*32)
        self.envelope=bootstrap_artifact.assemble(bytes(self.worker),64,1,self.source,bundle.blake(self.candidate),self.seed)
        for field in ['token','hostname','reserved']:
            with self.subTest(field=field):
                plan=bytearray(initial)
                if field=='token':plan[148:180]=bytes(32)
                elif field=='hostname':plan[180]=ord('A')
                else:plan[307]=1
                self.plan=initial;self.prepare()
                self.plan=bytes(plan)
                (self.root/'plan').write_bytes(self.plan)
                (self.root/'consent').write_bytes(b'PDDELIV1'+struct.pack('>II',64,1)+bundle.blake(self.envelope)+bundle.blake(self.plan))
                self.check(expected=3)

    def test_attempt_plan_is_bound_to_delivery_consent(self):
        self.plan=upgrade.encode(1,'4.3.16.42034','fixture-8600',bundle.blake(self.previous),
            bundle.blake(self.candidate),self.old_key,self.key,attempt=bytes([7])*32)
        self.prepare();self.check()
        stale=self.consent
        self.plan=upgrade.encode(1,'4.3.16.42034','fixture-8600',bundle.blake(self.previous),
            bundle.blake(self.candidate),self.old_key,self.key,attempt=bytes([8])*32)
        self.prepare()
        (self.root/'consent').write_bytes(stale)
        self.check(3)

    def test_install_has_its_distinct_fixed_descriptor_interface(self):
        target=approval.encode(1,'4.3.16.42034','fixture-8600',bundle.blake(self.candidate))
        self.plan=migration.encode(target,'a'*64,'b'*64,bytes(range(32)))
        self.envelope=bootstrap_artifact.assemble(self.worker,64,1,self.source,bundle.blake(self.candidate),self.seed)
        self.prepare();self.check()

    def test_consent_requires_private_permissions_and_single_link(self):
        path=self.root/'consent';path.chmod(0o644);self.check(3)
        path.chmod(0o600);os.link(path,self.root/'alias');self.check(3)

    def test_anonymous_private_consent_is_accepted(self):
        fd=os.memfd_create('pd-consent-test',os.MFD_CLOEXEC)
        try:
            os.write(fd,self.consent);os.fchmod(fd,0o600)
            command=self.command()
            p=subprocess.run(command,capture_output=True,text=True,pass_fds=[fd],timeout=10,
                env=dict(os.environ,PD_DELIVERY_ANONYMOUS_CONSENT=str(fd)))
            self.assertEqual(p.stderr,'');self.assertEqual(p.returncode,0,p.stdout)
            self.assertEqual(list(self.run.iterdir()),[])
        finally:os.close(fd)

    def test_consent_binds_entire_signed_envelope(self):
        worker=bytearray(self.worker);worker[-1]=1
        alternative=bootstrap_artifact.assemble(worker,64,2,self.source,bundle.blake(self.candidate),self.seed)
        (self.root/'artifact').write_bytes(alternative);self.check(3)

    def test_consent_binds_entire_plan(self):
        changed=bytearray(self.plan);changed[-1]^=1;(self.root/'plan').write_bytes(changed);self.check(3)

    def test_consent_header_and_length(self):
        for offset in [0,8,12,16,48]:
            with self.subTest(offset=offset):
                changed=bytearray(self.consent);changed[offset]^=1;(self.root/'consent').write_bytes(changed);self.check(3)
        for data in [self.consent[:-1],self.consent+b'x']:
            (self.root/'consent').write_bytes(data);self.check(3)

    def test_live_target_rechecked_before_exec(self):
        self.check(3,mode='wrong-target');self.check(3,mode='changed-target')

    def test_both_releases_must_authenticate(self):
        for name in ['candidate','previous']:
            original=(self.root/name).read_bytes();changed=bytearray(original);changed[-1]^=1
            (self.root/name).write_bytes(changed);self.check(3);(self.root/name).write_bytes(original)

    def test_publisher_key_is_not_taken_from_request(self):
        (self.root/'key').write_bytes(self.old_key);self.check(3)

    def test_worker_tampering_with_rebound_consent_still_refuses(self):
        bad=bytearray(self.envelope);bad[-1]^=1
        (self.root/'artifact').write_bytes(bad)
        (self.root/'consent').write_bytes(delivery_consent.encode(bad,self.plan));self.check(3)

    def test_foreign_stage_is_preserved(self):
        stage=self.run/'aetherpd-delivery';stage.mkdir();marker=stage/'foreign';marker.write_bytes(b'keep')
        self.check(3,clean=False);self.assertEqual(marker.read_bytes(),b'keep')

    def test_foreign_stage_symlink_is_preserved(self):
        foreign=self.root/'foreign';foreign.mkdir();stage=self.run/'aetherpd-delivery';stage.symlink_to(foreign)
        self.check(3,clean=False);self.assertTrue(stage.is_symlink());self.assertEqual(list(foreign.iterdir()),[])

    def test_worker_failure_and_timeout_clean_staging(self):
        self.check(4,mode='worker-failure');self.check(4,mode='hang')

    def test_cancel_reaps_owned_worker_then_cleans_staging(self):
        p=subprocess.Popen(self.command(),stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,
                           env=dict(os.environ,PD_DELIVERY_TEST_MODE='hang'))
        try:
            self.assertEqual(json.loads(p.stdout.readline()),{'verified_worker_interface':True})
            p.send_signal(signal.SIGTERM);stdout,stderr=p.communicate(timeout=5)
            self.assertEqual(p.returncode,4);self.assertEqual(stderr,'')
            self.assertEqual(json.loads(stdout),{'delivery_result':4});self.assertEqual(list(self.run.iterdir()),[])
        finally:
            if p.poll() is None:p.kill();p.wait()

    def test_foreign_file_prevents_claiming_cleanup_complete(self):
        self.check(5,mode='foreign-file',clean=False)
        self.assertEqual([p.name for p in (self.run/'aetherpd-delivery').iterdir()],['foreign'])

    def test_concurrent_delivery_cannot_remove_active_staging(self):
        p=subprocess.Popen(self.command(),stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,
                           env=dict(os.environ,PD_DELIVERY_TEST_MODE='hang'))
        try:
            self.assertEqual(json.loads(p.stdout.readline()),{'verified_worker_interface':True})
            self.check(3,clean=False)
            p.send_signal(signal.SIGTERM);stdout,stderr=p.communicate(timeout=5)
            self.assertEqual(p.returncode,4);self.assertEqual(stderr,'');self.assertEqual(list(self.run.iterdir()),[])
        finally:
            if p.poll() is None:p.kill();p.wait()

    def test_abrupt_parent_death_kills_worker_and_retry_recovers_exact_stage(self):
        p=subprocess.Popen(self.command(),stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,
                           env=dict(os.environ,PD_DELIVERY_TEST_MODE='hang'))
        childfd=None
        try:
            self.assertEqual(json.loads(p.stdout.readline()),{'verified_worker_interface':True})
            children=Path(f'/proc/{p.pid}/task/{p.pid}/children').read_text().split()
            self.assertEqual(len(children),1);childfd=os.pidfd_open(int(children[0]))
            poll=select.poll();poll.register(childfd,select.POLLIN)
            p.kill();p.communicate(timeout=5);self.assertEqual(p.returncode,-signal.SIGKILL)
            self.assertTrue(poll.poll(3000),'worker survived parent death')
            staged=(self.run/'aetherpd-delivery/worker').read_bytes()
            self.assertEqual(staged,self.worker);self.check()
        finally:
            if childfd is not None:os.close(childfd)
            if p.poll() is None:p.kill();p.wait()

    def stale_stage(self):
        stage=self.run/'aetherpd-delivery';stage.mkdir(mode=0o700)
        for name,data in [('receipt',self.consent),('worker',self.worker),('plan',self.plan),
                          ('candidate',self.candidate),('previous',self.previous)]:
            path=stage/name;path.write_bytes(data);path.chmod(0o700 if name=='worker' else 0o600)
        return stage

    def test_partial_staging_and_cleanup_resume_for_exact_delivery(self):
        for name in ['worker','plan','candidate','previous']:
            for length in [0,17]:
                with self.subTest(name=name,length=length):
                    stage=self.stale_stage();path=stage/name
                    path.write_bytes(path.read_bytes()[:length]);path.chmod(0o600)
                    self.check()
        stage=self.stale_stage();(stage/'worker').unlink();(stage/'candidate').unlink();self.check()

    def test_install_recovery_matches_embedded_approval(self):
        target=approval.encode(1,'4.3.16.42034','fixture-8600',bundle.blake(self.candidate))
        self.plan=migration.encode(target,'a'*64,'b'*64,bytes(range(32)))
        self.envelope=bootstrap_artifact.assemble(self.worker,64,1,self.source,bundle.blake(self.candidate),self.seed)
        self.prepare();stage=self.stale_stage();(stage/'previous').unlink()
        (stage/'approved').write_bytes(target[:23]);(stage/'approved').chmod(0o600);self.check()

    def test_empty_stage_without_receipt_is_preserved(self):
        stage=self.run/'aetherpd-delivery';stage.mkdir(mode=0o700)
        self.check(3,clean=False);self.assertTrue(stage.is_dir());self.assertEqual(list(stage.iterdir()),[])

    def test_changed_staged_bytes_refuse_without_removing_anything(self):
        for name in ['receipt','worker','plan','candidate','previous']:
            with self.subTest(name=name):
                stage=self.stale_stage();path=stage/name;data=bytearray(path.read_bytes());data[-1]^=1;path.write_bytes(data)
                before={p.name:p.read_bytes() for p in stage.iterdir()};self.check(3,clean=False)
                self.assertEqual(before,{p.name:p.read_bytes() for p in stage.iterdir()})
                for p in stage.iterdir():p.unlink()
                stage.rmdir()

    def test_missing_or_partial_receipt_cannot_authorize_recovery(self):
        for data in [None,b'',self.consent[:-1]]:
            stage=self.stale_stage();receipt=stage/'receipt'
            if data is None:receipt.unlink()
            else:receipt.write_bytes(data)
            self.check(3,clean=False);self.assertEqual((stage/'worker').read_bytes(),self.worker)
            for p in stage.iterdir():p.unlink()
            stage.rmdir()

    def test_foreign_entry_with_valid_receipt_preserves_whole_stage(self):
        stage=self.stale_stage();(stage/'foreign').write_bytes(b'keep');self.check(3,clean=False)
        self.assertEqual((stage/'worker').read_bytes(),self.worker)
        self.assertEqual((stage/'foreign').read_bytes(),b'keep')

    def test_stale_symlink_and_hardlink_and_mode_refuse(self):
        for kind in ['symlink','hardlink','mode']:
            stage=self.stale_stage();path=stage/'candidate'
            if kind=='mode':path.chmod(0o644)
            else:
                path.unlink()
                if kind=='symlink':path.symlink_to(self.root/'candidate')
                else:os.link(self.root/'candidate',path)
            self.check(3,clean=False);self.assertEqual((stage/'worker').read_bytes(),self.worker)
            for p in stage.iterdir():p.unlink()
            stage.rmdir()

    def test_orphan_worker_keeps_lease_until_it_exits(self):
        p=subprocess.Popen(self.command(),stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,
                           env=dict(os.environ,PD_DELIVERY_TEST_MODE='orphan'))
        child=None;childfd=None
        try:
            self.assertEqual(json.loads(p.stdout.readline()),{'verified_worker_interface':True})
            children=Path(f'/proc/{p.pid}/task/{p.pid}/children').read_text().split()
            self.assertEqual(len(children),1);child=int(children[0]);childfd=os.pidfd_open(child)
            poll=select.poll();poll.register(childfd,select.POLLIN)
            p.kill();p.wait(timeout=5);self.assertFalse(poll.poll(0))
            self.check(3,clean=False);os.kill(child,signal.SIGKILL)
            self.assertTrue(poll.poll(3000));child=None;p.communicate(timeout=5);self.check()
        finally:
            if child is not None:os.kill(child,signal.SIGKILL)
            if childfd is not None:os.close(childfd)
            if p.poll() is None:p.kill();p.wait()

    def test_encoder_refuses_other_target_and_noncanonical_plan(self):
        other=upgrade.encode(1,'4.3.16.42034','fixture-8600',bundle.blake(self.previous),bytes(32),self.old_key,self.key)
        for plan in [other,self.plan+b'x',b'unknown!'+self.plan[8:]]:
            with self.assertRaises(ValueError):delivery_consent.encode(self.envelope,plan)


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--checker',type=Path,required=True)
    args,rest=p.parse_known_args();CHECKER=args.checker.resolve();unittest.main(argv=[__file__,*rest])
