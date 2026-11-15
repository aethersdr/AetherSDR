#!/usr/bin/env python3
"""Run the real installer worker in a disposable user/network/mount namespace.

Radio, service and container adapters are fixture-linked; signatures, staging,
identity transfer, transactions, namespace leases and authority cleanup are real.
"""
import argparse
import fcntl
import hashlib
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
import approval
import bundle
import migration
import release
import units
import first_install
import json
CHECKER=None
SEED=hashlib.sha256(b'AetherPD whole-worker fixture, never a shipping trust anchor').digest()

class WorkerTest(unittest.TestCase):
 def setUp(self):
  self.temp=tempfile.TemporaryDirectory(prefix='pd-worker-');self.addCleanup(self.temp.cleanup);self.root=Path(self.temp.name)
  for path in ['fixture/old/tsnet','var/lib/smartsdr','run','etc/systemd/system','proc','dev']:(self.root/path).mkdir(parents=True,exist_ok=True,mode=0o755)
  for path in ['fixture/old','fixture/old/tsnet']:(self.root/path).chmod(0o700)
  self.fixture=self.root/'fixture';self.store=self.root/'var/lib/smartsdr/aetherpd';self.capsule=self.store.parent/'aetherpd-installation'
  shutil.copyfile(CHECKER,self.fixture/'bootstrap');(self.fixture/'bootstrap').chmod(0o700)
  shutil.copyfile(CHECKER.parent/'worker-namespace',self.fixture/'entry');(self.fixture/'entry').chmod(0o700)
  (self.fixture/'model').write_bytes(struct.pack('=5i',1,1,0,0,0));(self.fixture/'fault').write_text('')
  for path,data in [('old/provision.json',b'original-provision'),('old/tsnet/tailscaled.state',b'original-identity')]:
   p=self.fixture/path;p.write_bytes(data);p.chmod(0o600)
  raw=bytearray(4096);raw[:7]=b'\x7fELF\x02\x01\x01';struct.pack_into('<HH',raw,16,2,183)
  payload,_=bundle.assemble({n:bytes(raw) if n!='LICENSES' else b'fixture' for n in bundle.NAMES},64,bytes(32),SEED)
  files={'pd-boot':bytes(raw),'pd-update':bytes(raw),'payload.bundle':payload,'BOOT-LICENSES.txt':b'fixture',**{n:s.encode() for n,s in units.render('arm64').items()}}
  self.release,_=release.assemble(files,'flex8600','4.3.16.42034',bytes(32),SEED)
  self.approval=approval.encode(1,'4.3.16.42034','fixture-8600',bundle.blake(self.release))
  self.plan=migration.encode(self.approval,'a'*64,'b'*64,bytes(range(32)))
  for name,data in [('release',self.release),('approved',self.approval),('plan',self.plan)]:
   p=self.fixture/name;p.write_bytes(data);p.chmod(0o600)
 def state(self):return dict(zip(['old_present','old_running','new_running','enabled','launched'],struct.unpack('=5i',(self.fixture/'model').read_bytes())))
 def events(self):
  p=self.fixture/'events';return p.read_text().splitlines() if p.exists() else []
 def invoke(self,mode='forward',fault='',expected=0,alternate=None):
  (self.fixture/'fault').write_text(fault)
  selected='copy' if alternate else mode
  cmd=['bwrap','--unshare-user','--uid','0','--gid','0','--unshare-pid','--unshare-net','--unshare-ipc','--unshare-uts',
       '--bind',str(self.root),'/','--proc','/proc','--dev','/dev','--die-with-parent','--new-session','--clearenv','--chdir','/','/fixture/entry',selected]
  result=subprocess.run(cmd,stdin=subprocess.DEVNULL,capture_output=True,timeout=20)
  self.assertEqual(result.returncode,expected,result.stderr.decode(errors='replace')+'\nevents='+str(self.events()))
  return result
 def clean(self):
  self.assertFalse(self.capsule.exists());self.assertFalse((self.store.parent/'.aetherpd-installation-retired').exists())
  self.assertFalse((self.root/'run/aetherpd-install').exists())
  directory=self.root/'etc/systemd/system'
  for name in ['aetherpd-install.service','aetherpd-recover.service','multi-user.target.wants/aetherpd-recover.service']:self.assertFalse((directory/name).is_symlink() or (directory/name).exists())
 def assert_original(self,rotated=False):
  state=self.state();self.assertEqual([state[k] for k in ['old_present','old_running','new_running','enabled']],[1,1,0,0])
  self.assertEqual((self.fixture/'old/tsnet/tailscaled.state').read_bytes(),b'rotated-identity' if rotated else b'original-identity')
  self.assertFalse((self.root/'run/aetherpd').exists());self.clean()
 def test_bootstrap_retry_after_worker_stop_queues_recovery(self):
  self.invoke('bootstrap');self.invoke(fault='kill:stop:after',expected=137)
  self.invoke('bootstrap');self.invoke();self.assert_original()
 def test_bootstrap_retry_after_activation_queues_recovery(self):
  self.invoke('bootstrap');self.invoke(fault='kill:activate:after',expected=137)
  self.invoke('bootstrap');self.invoke();self.assert_original(rotated=True)
 def test_bootstrap_recovers_partial_extraction(self):
  self.invoke('bootstrap',fault='kill:stage:extracted',expected=137)
  partial=self.root/'run/aetherpd-install.stage/payload.bundle';data=partial.read_bytes();partial.write_bytes(data[:117]);partial.chmod(0o600)
  self.invoke('bootstrap');self.invoke();self.clean()
 def test_bootstrap_preserves_foreign_partial_staging(self):
  partial=self.root/'run/aetherpd-install.stage';partial.mkdir(mode=0o700);(partial/'pd-boot').write_bytes(b'foreign');(partial/'pd-boot').chmod(0o600)
  self.invoke('bootstrap',expected=4);self.assertEqual((partial/'pd-boot').read_bytes(),b'foreign');self.assertFalse(self.store.exists())
 def test_bootstrap_recovers_after_stage_publication(self):
  self.invoke('bootstrap',fault='kill:stage:renamed',expected=137);self.invoke('bootstrap');self.invoke();self.clean()
 def test_bootstrap_recovers_partial_persistent_preparation(self):
  self.invoke('bootstrap',fault='kill:install:fixed-files-written',expected=137);self.invoke('bootstrap');self.invoke();self.clean()
 def test_bootstrap_recovers_capsule_before_units_armed(self):
  self.invoke('bootstrap',fault='kill:capsule:capsule-rename',expected=137);self.invoke('bootstrap');self.invoke();self.clean()
 def test_bootstrap_reconciles_detached_terminal_capsule(self):
  self.invoke('bootstrap');self.invoke(fault='kill:capsule:retire-rename',expected=137);self.invoke('bootstrap');self.clean();self.assertEqual(self.state()['new_running'],1)
 def test_bootstrap_lease_refuses_before_preparation(self):
  lock=self.root/'run/.aetherpd-bootstrap.lock';lock.touch(mode=0o600)
  with lock.open('r+b') as held:
   fcntl.flock(held,fcntl.LOCK_EX);self.invoke('bootstrap',expected=4)
  self.assertFalse(self.store.exists());self.assertFalse((self.root/'run/aetherpd-install').exists())
 def test_running_worker_lease_blocks_bootstrap_reentry(self):
  self.invoke('bootstrap');lock=self.store.parent/'.aetherpd-installation.lock'
  before=(self.store/'active').read_bytes()
  with lock.open('r+b') as held:
   fcntl.flock(held,fcntl.LOCK_EX);self.invoke('bootstrap',expected=4)
  self.assertEqual((self.store/'active').read_bytes(),before);self.assertNotIn('stop:before',self.events())
 def test_transient_target_probe_failure_is_retryable(self):
  self.invoke('bootstrap');self.invoke(fault='fail:identity',expected=4);self.assertEqual(self.state()['old_running'],1)
  self.invoke();self.clean()
 def test_bootstrap_returns_without_stopping_predecessor(self):
  self.invoke('bootstrap');self.assertEqual(self.state()['old_running'],1);self.assertNotIn('stop:before',self.events())
  self.assertTrue(self.capsule.exists());self.assertTrue((self.root/'etc/systemd/system/multi-user.target.wants/aetherpd-recover.service').is_symlink())
 def test_migration_without_predecessor_is_refused(self):
  # A legacy migration intent, valid signed release and
  # approval must not turn a missing predecessor into a fresh installation.
  shutil.rmtree(self.fixture/'old')
  (self.fixture/'model').write_bytes(struct.pack('=5i',0,0,0,0,0))
  self.invoke('bootstrap',expected=3)
  self.assertEqual(list(self.state().values()),[0,0,0,0,0])
  self.assertEqual(self.events(),['identity'])
  self.assertFalse(self.store.exists());self.clean()
  self.assertEqual(list((self.root/'etc/systemd/system').iterdir()),[])
  self.assertEqual(list((self.root/'var/lib/smartsdr').iterdir()),[])
 def fresh(self):
  shutil.rmtree(self.fixture/'old')
  (self.fixture/'model').write_bytes(struct.pack('=5i',0,0,0,0,0))
  self.plan=first_install.encode(self.approval,hashlib.sha256(b'private fixture token').digest(),'aetherpd-fixture',bytes(range(32)))
  (self.fixture/'plan').write_bytes(self.plan)
 def test_explicit_fresh_install_initializes_local_identity(self):
  self.fresh();self.invoke('bootstrap');self.invoke();self.clean()
  self.assertEqual(list(self.state().values()),[0,0,1,1,1])
  config=json.loads((self.store/'state/provision.json').read_text())
  self.assertEqual(config['token_sha256'],hashlib.sha256(b'private fixture token').hexdigest())
  self.assertEqual(config['installation_id'],bytes(range(32)).hex());self.assertTrue(config['local_only'])
  self.assertFalse((self.store/'state/tsnet').exists())
  self.assertFalse(any(x.startswith(('stop:','retire:','restore:','recover:')) for x in self.events()))
 def test_fresh_install_refuses_existing_predecessor(self):
  self.fresh();(self.fixture/'model').write_bytes(struct.pack('=5i',1,1,0,0,0))
  self.invoke('bootstrap',expected=3);self.assertFalse(self.store.exists())
 def test_fresh_install_preserves_foreign_identity(self):
  self.fresh();self.invoke('bootstrap')
  config=self.store/'state/provision.json';config.write_bytes(b'foreign identity');config.chmod(0o600)
  self.invoke();self.clean();self.assertEqual(config.read_bytes(),b'foreign identity')
  self.assertEqual(self.state()['enabled'],0);self.assertEqual(self.state()['new_running'],0)
 def test_fresh_recovery_never_reconstructs_predecessor(self):
  self.fresh();self.invoke('bootstrap');self.invoke(fault='kill:activate:after',expected=137)
  identity=(self.store/'state/provision.json').read_bytes()
  self.invoke('recover');self.clean()
  self.assertEqual(self.state()['new_running'],0);self.assertEqual(self.state()['enabled'],0)
  self.assertEqual((self.store/'state/provision.json').read_bytes(),identity)
  self.assertFalse(any(x.startswith(('restore:','recover:')) for x in self.events()))
 def test_fresh_attempt_cannot_adopt_other_prepared_store(self):
  self.fresh();self.invoke('bootstrap',fault='kill:install:store-published',expected=137)
  prior=(self.store/'intent').read_bytes()
  self.plan=first_install.encode(self.approval,hashlib.sha256(b'other token').digest(),'other',b'n'*32)
  (self.fixture/'plan').write_bytes(self.plan)
  self.invoke('bootstrap',expected=3);self.assertEqual((self.store/'intent').read_bytes(),prior)
 def test_fresh_preparation_retries_same_attempt(self):
  self.fresh();self.invoke('bootstrap',fault='kill:install:store-published',expected=137)
  self.invoke('bootstrap');self.invoke();self.clean();self.assertEqual(self.state()['enabled'],1)
 def test_fresh_preparation_recovers_unpublished_intent_prefix(self):
  self.fresh();pending=self.store.parent/'.aetherpd-prepare';pending.mkdir(mode=0o700)
  marker=pending/'intent.new';marker.write_bytes(bundle.blake(self.plan)[:13]);marker.chmod(0o600)
  self.invoke('bootstrap');self.invoke();self.clean();self.assertEqual(self.state()['enabled'],1)
 def test_fresh_partial_identity_publication_restarts_safely(self):
  self.fresh();self.invoke('bootstrap')
  partial=self.store/'state/.first-provision';partial.write_bytes(b'');partial.chmod(0o600)
  self.invoke();self.clean();self.assertTrue(json.loads((self.store/'state/provision.json').read_text())['local_only'])
 def test_forward_commit_removes_temporary_authority(self):
  self.invoke('bootstrap');self.invoke();state=self.state();self.assertEqual([state[k] for k in ['old_present','old_running','new_running','enabled']],[0,0,1,1]);self.clean()
  self.assertEqual((self.store/'transaction').read_bytes(),b'PDTXN001'+self.plan+struct.pack('>I',5))
 def test_recovery_before_first_transaction_never_goes_forward(self):
  self.invoke('bootstrap');self.invoke('recover');self.assert_original();self.assertNotIn('stop:before',self.events());self.assertNotIn('activate:before',self.events())
 def test_executable_copy_cannot_impersonate_capsule_worker(self):
  self.invoke('bootstrap');self.invoke(alternate='/fixture/bootstrap',expected=3);self.assertEqual(self.state()['old_running'],1);self.assertNotIn('stop:before',self.events())
 def test_modified_signed_capsule_refuses_before_mutation(self):
  self.invoke('bootstrap');p=self.capsule/'release';data=bytearray(p.read_bytes());data[-1]^=1;p.write_bytes(data)
  self.invoke(expected=3);self.assertNotIn('stop:before',self.events())
 def test_returned_failure_after_stop_restores_original(self):
  self.invoke('bootstrap');self.invoke(fault='fail:stop:after');self.assert_original()
 def test_returned_failure_after_retirement_reconstructs(self):
  self.invoke('bootstrap');self.invoke(fault='fail:retire:after');self.assert_original()
 def test_returned_failure_after_activation_preserves_latest_identity(self):
  self.invoke('bootstrap');self.invoke(fault='fail:activate:after');self.assert_original(rotated=True)
 def test_worker_death_after_stop_recovers(self):
  self.invoke('bootstrap');self.invoke(fault='kill:stop:after',expected=137);self.invoke('recover');self.assert_original()
 def test_worker_death_after_retirement_recovers(self):
  self.invoke('bootstrap');self.invoke(fault='kill:retire:after',expected=137);self.invoke('recover');self.assert_original()
 def test_worker_death_after_activation_recovers_latest_identity(self):
  self.invoke('bootstrap');self.invoke(fault='kill:activate:after',expected=137);self.invoke('recover');self.assert_original(rotated=True)
 def test_worker_death_after_autostart_enable_rolls_back(self):
  self.invoke('bootstrap');self.invoke(fault='kill:enable:after',expected=137);self.assertEqual(self.state()['enabled'],1);self.invoke('recover');self.assert_original(rotated=True)
 def test_terminal_commit_retry_only_cleans(self):
  self.invoke('bootstrap');self.invoke(fault='kill:transaction:durable:5',expected=137);before=self.events().count('activate:before');self.invoke('recover');self.clean()
  self.assertEqual(self.events().count('activate:before'),before);self.assertEqual(self.state()['new_running'],1)
 def test_interrupted_runtime_removal_resumes(self):
  self.invoke('bootstrap');self.invoke(fault='kill:activate:after',expected=137)
  self.invoke('recover',fault='kill:runtime:unlink',expected=137);self.invoke('recover');self.assert_original(rotated=True)
 def test_unverified_quiescence_prevents_reconstruction(self):
  self.invoke('bootstrap');self.invoke(fault='kill:activate:after',expected=137)
  self.invoke('recover',fault='fail:quiesce',expected=4);self.assertEqual(self.state()['old_present'],0)
  self.invoke('recover');self.assert_original(rotated=True)
if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('--checker',type=Path,required=True);a,rest=p.parse_known_args();CHECKER=a.checker.resolve();unittest.main(argv=[__file__,*rest])
