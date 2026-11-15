#!/usr/bin/env python3
"""Real filesystem rollback cleanup, foreign-object preservation and kill recovery."""
import argparse
import os
from pathlib import Path
import signal
import socket
import struct
import subprocess
import tempfile
import unittest
import approval
import migration
import upgrade
CHECKER=None
NAMES=['flex-tailnet-shim','aether-pd','pd-primary-launch','pd-start-session','pd-radio-launch','pd-fixed-arp','pd-iq-source','pd-capture-bootstrap','pd-unkey-guard','pd-lease-guard','LICENSES']
class RuntimeTest(unittest.TestCase):
 def setUp(self):
  self.temp=tempfile.TemporaryDirectory(prefix='pd-runtime-');self.addCleanup(self.temp.cleanup);self.root=Path(self.temp.name)
  self.store=self.root/'store';self.store.mkdir();(self.root/'run').mkdir();self.runtime=self.root/'run/aetherpd'
  self.plan=migration.encode(approval.encode(1,'4.3.16.42034','fixture-8600',bytes(range(32))),'a'*64,'b'*64,bytes(range(32)))
  self.record=self.root/'approved';self.record.write_bytes(self.plan);self.record.chmod(0o600);self.phase(6)
  (self.store/'state').mkdir();(self.store/'state/provision.json').write_bytes(b'persistent identity must survive')
  self.populate()
 def phase(self,n):
  p=self.store/'transaction';p.write_bytes(b'PDTXN001'+self.plan+struct.pack('>I',n));p.chmod(0o600)
 def populate(self):
  self.runtime.mkdir();(self.runtime/'bin').mkdir();(self.runtime/'.stage').mkdir(mode=0o700)
  for n in NAMES:
   p=self.runtime/'bin'/n;p.write_bytes(b'runtime');p.chmod(0o755 if n!='LICENSES' else 0o644)
  for d in ['primary','primary/tsnet','bin/state','bin/state/tsnet','bin/run','bin/run/tsnet']:
   p=self.runtime/d;p.mkdir(mode=0o700)
  for base in ['primary/tsnet','bin/state/tsnet','bin/run/tsnet']:
   for tail in ['profile-data','profile-data/8fd1','profile-data/8fd1/netmap-cache']:
    (self.runtime/base/tail).mkdir(mode=0o700)
   p=self.runtime/base/'profile-data/8fd1/netmap-cache/73656c66';p.write_bytes(b'volatile netmap');p.chmod(0o600)
  for n in ['primary/tsnet/log.conf','bin/state/provision.json','bin/run/tsnet/cache']:
   p=self.runtime/n;p.write_bytes(b'volatile');p.chmod(0o600)
  sock=socket.socket(socket.AF_UNIX);sock.bind(str(self.runtime/'start.sock'));sock.close();(self.runtime/'start.sock').chmod(0o600)
 def invoke(self,expected=0,kill=None):
  p=subprocess.run([str(CHECKER),str(self.root),str(self.record)]+([] if kill is None else [str(kill)]),capture_output=True,timeout=10)
  self.assertEqual(p.returncode,expected,p.stderr.decode(errors='replace'))
  self.assertEqual((self.store/'state/provision.json').read_bytes(),b'persistent identity must survive')
 def snapshot(self):
  return sorted((str(p.relative_to(self.runtime)),p.lstat().st_mode,p.lstat().st_ino) for p in self.runtime.rglob('*'))
 def refused(self):
  before=self.snapshot();self.invoke(1);self.assertEqual(self.snapshot(),before)
 def test_complete_tree_removed_and_repeat_is_idempotent(self):
  self.invoke();self.assertFalse(self.runtime.exists());self.invoke()
 def test_all_other_phases_refuse_without_deletion(self):
  for phase in [0,1,2,3,4,5,7,8]:self.phase(phase);self.refused()
 def test_live_owned_runtime_refuses(self):
  (self.root/'busy').touch();self.refused()
 def test_unknown_top_and_binary_names_preserved(self):
  for name in ['foreign','bin/foreign','.stage/foreign']:
   p=self.runtime/name;p.write_bytes(b'foreign');self.refused();p.unlink()
 def test_links_do_not_escape_runtime(self):
  target=self.store/'state/provision.json'
  for name in ['primary/tsnet/log.conf','bin/aether-pd']:
   p=self.runtime/name;data=p.read_bytes();mode=p.stat().st_mode;p.unlink();p.symlink_to(target);self.refused();p.unlink()
   os.link(target,p);self.refused();p.unlink();p.write_bytes(data);p.chmod(mode)
 def test_directory_symlinks_refused(self):
  p=self.runtime/'.stage';p.rmdir();p.symlink_to(self.store);self.refused()
 def test_root_alias_refused(self):
  moved=self.root/'elsewhere';self.runtime.rename(moved);self.runtime.symlink_to(moved);self.invoke(1);self.assertTrue((moved/'bin/aether-pd').exists())
 def test_special_objects_refused(self):
  p=self.runtime/'primary/tsnet/fifo';os.mkfifo(p,0o600);self.refused()
 def test_extra_nested_directory_refused(self):
  (self.runtime/'primary/tsnet/extra').mkdir(mode=0o700);self.refused()
 def test_profile_cache_rejects_foreign_layout(self):
  for name in ['primary/tsnet/profile-data/not-a-profile','primary/tsnet/profile-data/abcd/foreign','primary/tsnet/profile-data/8fd1/netmap-cache/nested']:
   p=self.runtime/name;p.mkdir(mode=0o700,parents=True);self.refused();p.rmdir()
 def test_profile_cache_links_preserve_persistent_state(self):
  p=self.runtime/'primary/tsnet/profile-data/8fd1/netmap-cache/73656c66'
  p.unlink();p.symlink_to(self.store/'state/provision.json');self.refused();p.unlink()
  os.link(self.store/'state/provision.json',p);self.refused()
 def test_privilege_or_writable_modes_refused(self):
  for name,mode in [('bin/aether-pd',0o4755),('bin/aether-pd',0o777),('primary/tsnet/log.conf',0o644),('primary',0o755),('bin',0o777)]:
   p=self.runtime/name;old=p.stat().st_mode;p.chmod(mode);self.refused();p.chmod(old)
 def test_oversized_leaf_refused(self):
  p=self.runtime/'primary/tsnet/log.conf'
  with p.open('r+b') as f:f.truncate(16*1024*1024+1)
  self.refused()
 def test_entry_limit_refuses_before_deleting_anything(self):
  for i in range(256):
   p=self.runtime/'primary/tsnet'/str(i);p.write_bytes(b'');p.chmod(0o600)
  self.refused()
 def test_kill_after_each_unlink_resumes(self):
  total=len(self.snapshot())
  for point in range(1,total+1):
   self.invoke(-signal.SIGKILL,point);self.invoke();self.assertFalse(self.runtime.exists());self.populate()
 def test_installation_status_and_partial_are_removable(self):
  self.write_status('installation',204,0o444)
  self.write_status('.installation.new',73,0o600)
  self.invoke();self.assertFalse(self.runtime.exists())
 def write_status(self,name,size,mode):
  p=self.runtime/name;p.write_bytes(bytes(size));p.chmod(mode)
 def test_invalid_installation_status_preserves_runtime(self):
  for name,size,mode in [('installation',203,0o444),('installation',204,0o644),('.installation.new',205,0o600),('.installation.new',1,0o644)]:
   self.write_status(name,size,mode);self.refused();(self.runtime/name).unlink()
 def test_partial_stage_is_removable(self):
  p=self.runtime/'.stage/aether-pd';p.write_bytes(b'partial');p.chmod(0o600);self.invoke()

class UpgradeRuntimeTest(RuntimeTest):
 def setUp(self):
  self.upgrade=None
  super().setUp()
  self.phase(5)
  self.upgrade=upgrade.encode(1,'4.3.16.42034','fixture-8600',bytes(range(32)),bytes([9])*32,bytes([2])*32,bytes([3])*32)
  self.record.write_bytes(self.upgrade)
  for name,data in [('migration-approved',self.plan),('transaction.lock',b'')]:
   p=self.store/name;p.write_bytes(data);p.chmod(0o600)
  (self.root/'aetherpd-upgrade').mkdir(mode=0o700)
  self.phase(1)
 def phase(self,n):
  if self.upgrade is None:return super().phase(n)
  p=self.store/'upgrade-transaction';p.write_bytes(b'PDUTXN01'+self.upgrade+struct.pack('>I',n));p.chmod(0o600)
 def test_all_other_phases_refuse_without_deletion(self):
  for phase in [0,2,3,4,6,7,8]:self.phase(phase);self.refused()
 def test_rollback_phase_retires_candidate_runtime(self):
  self.phase(5);self.invoke();self.assertFalse(self.runtime.exists())
 def test_missing_worker_cannot_retire_runtime(self):
  (self.root/'no-worker').touch();self.refused()
 def test_mismatched_approval_preserves_runtime(self):
  changed=bytearray(self.upgrade);changed[-1]^=1;self.record.write_bytes(changed);self.refused()
 def test_partial_upgrade_journal_preserves_runtime(self):
  p=self.store/'upgrade-transaction.new';p.write_bytes(b'');p.chmod(0o600);self.refused()
 def test_original_installation_still_must_be_committed(self):
  original=(self.store/'transaction').read_bytes()
  for phase in [1,2,3,4,6,7]:
   (self.store/'transaction').write_bytes(original[:-1]+bytes([phase]));self.refused()
 def test_missing_capsule_preserves_runtime(self):
  (self.root/'aetherpd-upgrade').rmdir();self.refused()

if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('--checker',type=Path,required=True);args,rest=p.parse_known_args();CHECKER=args.checker.resolve();unittest.main(argv=[__file__,*rest])
