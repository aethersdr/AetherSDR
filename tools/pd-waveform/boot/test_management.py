#!/usr/bin/env python3
"""Real fixed-file replacement under approved upgrade state and process death."""
import argparse
import hashlib
import os
from pathlib import Path
import signal
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
import upgrade
import management_blob
CHECKER=None
VERIFIER=None

class ManagementTest(unittest.TestCase):
    origin_recovery=True
    generation_selection=False
    candidate_index=1
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(prefix='pd-management-');self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name);self.store=self.root/'store';self.units=self.root/'units'
        for p in [self.store,self.store/'units',self.units,self.root/'aetherpd-upgrade']:p.mkdir(mode=0o700)
        self.files=[];artifacts=[];keys=[]
        for i in [0,self.candidate_index]:
            seed=hashlib.sha256(('management fixture '+str(i)).encode()).digest()
            keys.append(Ed25519PrivateKey.from_private_bytes(seed).public_key().public_bytes_raw())
            elf=bytearray(4096);elf[:7]=b'\x7fELF\x02\x01\x01';struct.pack_into('<HH',elf,16,2,183);elf[64]=i
            payload,_=bundle.assemble({n:bytes(elf) if n!='LICENSES' else b'fixture' for n in bundle.NAMES},64,bytes([i])*32,seed)
            files={'pd-boot':bytes(elf),'pd-update':bytes(elf),'payload.bundle':payload,'BOOT-LICENSES.txt':('fixture licenses '+str(i)).encode(),
                   **{n:s.encode() for n,s in units.render('arm64',bool(i)).items()}}
            artifact,_=release.assemble(files,'flex8600','4.3.16.42034',bytes([i])*32,seed,paired_boot=bool(i),managed_startup=bool(i),origin_recovery=bool(i) and self.origin_recovery,generation_selection=bool(i) and self.generation_selection)
            self.files.append(files);artifacts.append(artifact)
        self.artifacts=artifacts;self.keys=keys
        self.approved=upgrade.encode(1,'4.3.16.42034','fixture-8600',*map(bundle.blake,artifacts),*keys)
        for name,data in [('approved',self.approved),('previous.header',artifacts[0][:release.PREFIX]),('candidate.header',artifacts[1][:release.PREFIX]),
                          ('candidate.digest',bundle.blake(artifacts[1])),('boot',self.files[1]['pd-boot']),('licenses',self.files[1]['BOOT-LICENSES.txt'])]:self.put(self.root/name,data)
        plan=migration.encode(approval.encode(1,'4.3.16.42034','fixture-8600',bundle.blake(artifacts[0])),'a'*64,'b'*64,bytes(range(32)))
        self.put(self.store/'migration-approved',plan);self.put(self.store/'transaction',b'PDTXN001'+plan+struct.pack('>I',5));self.put(self.store/'transaction.lock',b'')
        self.phase(1);self.targets=[]
        for name in ['pd-boot','BOOT-LICENSES.txt']:self.targets.append((self.store/name,name))
        for directory in [self.store/'units',self.units]:
            for name in units.render('arm64'):self.targets.append((directory/name,name))
        self.reset_targets()
        (self.store/'state').mkdir(mode=0o700);self.put(self.store/'state/private-fixture',b'preserve identity')
    def put(self,path,data,mode=0o600):path.write_bytes(data);path.chmod(mode)
    def phase(self,phase):self.put(self.store/'upgrade-transaction',b'PDUTXN01'+self.approved+struct.pack('>I',phase))
    def reset_targets(self):
        for p,name in self.targets:
            self.put(p,self.files[0][name],0o755 if name=='pd-boot' else 0o644)
            p.with_name('.'+p.name+'.management-new').unlink(missing_ok=True)
    def invoke(self,fault='',expected=0):
        r=subprocess.run([str(CHECKER),str(self.root),fault],capture_output=True,timeout=10)
        self.assertEqual(r.returncode,expected,r.stderr.decode(errors='replace'))
    def check(self):
        for p,name in self.targets:
            self.assertEqual(p.read_bytes(),self.files[1][name]);self.assertFalse(p.with_name('.'+p.name+'.management-new').exists())
        self.assertEqual((self.store/'state/private-fixture').read_bytes(),b'preserve identity')
    def snapshot(self):return {str(p.relative_to(self.root)):p.read_bytes() for p,_ in self.targets}
    def test_complete_and_idempotent_transition(self):
        self.invoke();self.check();inodes=[p.stat().st_ino for p,_ in self.targets]
        self.invoke();self.check();self.assertEqual(inodes,[p.stat().st_ino for p,_ in self.targets])
    def retained_management(self):
        seed=hashlib.sha256(b'rolled back management fixture').digest()
        key=Ed25519PrivateKey.from_private_bytes(seed).public_key().public_bytes_raw()
        raw=bytearray(self.files[1]['pd-boot']);raw[64]=2
        payload,_=bundle.assemble({n:bytes(raw) if n!='LICENSES' else b'fixture' for n in bundle.NAMES},64,bytes([2])*32,seed)
        files=dict(self.files[1]);files.update({'pd-boot':bytes(raw),'payload.bundle':payload,'BOOT-LICENSES.txt':b'retained management'})
        artifact,_=release.assemble(files,'flex8600','4.3.16.42034',bytes([2])*32,seed,paired_boot=True,managed_startup=True)
        prior=upgrade.encode(1,'4.3.16.42034','fixture-8600',bundle.blake(self.artifacts[0]),bundle.blake(artifact),self.keys[0],key)
        origin=b'PDORIG01'+bundle.blake(self.approved)+b'PDUTXN01'+prior+struct.pack('>I',7)+artifact[:release.PREFIX]
        self.retained_files=files;self.retained_artifact=artifact;self.retained_key=key;self.retained_approval=prior
        self.put(self.root/'origin',origin)
        for p,name in self.targets:self.put(p,files[name],0o755 if name=='pd-boot' else 0o644)
        return origin
    def test_retained_management_from_rolled_back_other_key(self):
        self.retained_management();self.invoke();self.check();self.invoke();self.check()
    def legacy_candidate(self):
        seed=hashlib.sha256(b'management fixture 1').digest()
        artifact,_=release.assemble(self.files[1],'flex8600','4.3.16.42034',bytes([1])*32,seed,managed_startup=True)
        self.approved=upgrade.encode(1,'4.3.16.42034','fixture-8600',bundle.blake(self.artifacts[0]),bundle.blake(artifact),*self.keys)
        self.put(self.root/'approved',self.approved);self.phase(1)
        self.put(self.root/'candidate.header',artifact[:release.PREFIX]);self.put(self.root/'candidate.digest',bundle.blake(artifact))
    def test_origin_refuses_candidate_without_permanent_cleanup_contract(self):
        self.legacy_candidate();self.retained_management();before=self.snapshot()
        self.invoke(expected=1);self.assertEqual(before,self.snapshot())
    def test_legacy_management_without_origin_still_supported(self):
        self.legacy_candidate();self.invoke();self.check()
    def test_retained_management_without_origin_refuses_before_writes(self):
        self.retained_management();(self.root/'origin').unlink()
        before=self.snapshot();self.invoke(expected=1);self.assertEqual(before,self.snapshot())
    def test_origin_wrong_approval_lineage_signature_and_length_refuse(self):
        origin=self.retained_management();before=self.snapshot()
        for offset in [0,8,40,40+8+8+76,40+363,len(origin)-1]:
            bad=bytearray(origin);bad[offset]^=1;self.put(self.root/'origin',bad)
            self.invoke(expected=1);self.assertEqual(before,self.snapshot())
        for bad in [origin[:-1],origin+b'\0']:
            self.put(self.root/'origin',bad);self.invoke(expected=1);self.assertEqual(before,self.snapshot())
    def test_retained_management_replacement_interruptions_resume(self):
        for index in [0,1]:
            for point in ['file','rename','durable']:
                self.retained_management();self.invoke('kill:'+point+':'+str(index),-signal.SIGKILL)
                self.invoke();self.check()
    def test_overlay_unit_devices_preserve_authenticated_replacement(self):
        self.invoke('overlay');self.check();self.invoke('overlay');self.check()
    def test_foreign_unit_filesystem_refuses_before_replacement(self):
        for fault in ['foreign-filesystem','foreign-overlay']:
            before=self.snapshot();self.invoke(fault,1);self.assertEqual(before,self.snapshot())
    def test_each_interruption_resumes_without_original_inputs(self):
        for phase in [1,5]:
            self.phase(phase)
            for i in range(10):
                name=self.targets[i][1]
                if self.files[0][name]==self.files[1][name]:continue
                for point in ['file','rename','durable']:
                    with self.subTest(phase=phase,index=i,point=point):
                        self.reset_targets();self.invoke('kill:'+point+':'+str(i),-signal.SIGKILL)
                        self.invoke();self.check()
    def test_every_foreign_destination_prevents_all_writes(self):
        for p,name in self.targets:
            self.reset_targets();p.write_bytes(b'foreign');before=self.snapshot()
            self.invoke(expected=1);self.assertEqual(before,self.snapshot())
    def test_foreign_partial_and_unit_dropin_prevent_all_writes(self):
        for p,_ in self.targets:
            partial=p.with_name('.'+p.name+'.management-new');self.put(partial,b'foreign');before=self.snapshot()
            self.invoke(expected=1);self.assertEqual(before,self.snapshot());self.assertEqual(partial.read_bytes(),b'foreign');partial.unlink()
        dropin=self.units/'aetherpd-shim.service.d';dropin.mkdir()
        before=self.snapshot();self.invoke(expected=1);self.assertEqual(before,self.snapshot())
    def test_matching_partial_prefixes_resume(self):
        for p,name in self.targets:
            self.reset_targets();data=self.files[1][name]
            partial=p.with_name('.'+p.name+'.management-new');self.put(partial,data[:len(data)//2])
            self.invoke();self.check()
    def test_unsafe_destination_metadata_is_preserved(self):
        for p,name in self.targets:
            self.reset_targets();p.chmod(0o666);self.invoke(expected=1)
            p.chmod(0o755 if name=='pd-boot' else 0o644);alias=self.root/'alias';os.link(p,alias)
            self.invoke(expected=1);alias.unlink();raw=p.read_bytes();p.unlink();self.put(alias,raw,0o755 if name=='pd-boot' else 0o644);p.symlink_to(alias)
            self.invoke(expected=1);self.assertTrue(p.is_symlink());p.unlink();alias.rename(p)
    def test_space_refusal_precedes_any_replacement(self):
        before=self.snapshot();self.invoke('no-space',1);self.assertEqual(before,self.snapshot())
        self.assertFalse(any(p.name.endswith('.management-new') for p in self.root.rglob('*')))

    def test_wrong_candidate_bytes_header_and_digest_refuse(self):
        for name in ['boot','licenses','candidate.header','candidate.digest','previous.header']:
            p=self.root/name;old=p.read_bytes();changed=bytearray(old);changed[-1]^=1;p.write_bytes(changed)
            before=self.snapshot();self.invoke(expected=1);self.assertEqual(before,self.snapshot());p.write_bytes(old)
    def test_journal_phase_capsule_approval_and_live_worker_required(self):
        before=self.snapshot()
        for phase in [0,2,3,4,6,7,8]:self.phase(phase);self.invoke(expected=1)
        self.phase(1);self.invoke('no-lock',1)
        (self.root/'aetherpd-upgrade').rmdir();self.invoke(expected=1);(self.root/'aetherpd-upgrade').mkdir(mode=0o700)
        p=self.store/'upgrade-transaction';data=bytearray(p.read_bytes());data[20]^=1;p.write_bytes(data);self.invoke(expected=1)
        self.assertEqual(before,self.snapshot())
    def test_embedded_management_is_verified_reproducible_and_compiles(self):
        artifact=self.root/'candidate.release';artifact.write_bytes(self.artifacts[1]);output=self.root/'embedded.h'
        report=management_blob.emit(artifact,self.keys[1],'arm64',output,VERIFIER)
        self.assertEqual(report['release_blake2b_256'],bundle.blake(self.artifacts[1]).hex())
        original=output.read_bytes();management_blob.emit(artifact,self.keys[1],'arm64',output,VERIFIER)
        self.assertEqual(output.read_bytes(),original)
        # Compile and consume the generated object, rather than just checking text.
        program=self.root/'consume.c';program.write_text('#include "embedded.h"\n#include <stdio.h>\nint main(void){fwrite(pd_management_embedded.boot,1,pd_management_embedded.boot_size,stdout);fwrite(pd_management_embedded.licenses,1,pd_management_embedded.license_size,stdout);fwrite(pd_management_embedded.header,1,sizeof(pd_management_embedded.header),stdout);fwrite(pd_management_embedded.release_digest,1,32,stdout);return 0;}\n')
        binary=self.root/'consume';subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-I',str(Path(__file__).parent),str(program),'-o',str(binary)],check=True,capture_output=True)
        r=subprocess.run([str(binary)],check=True,capture_output=True)
        self.assertEqual(r.stdout,self.files[1]['pd-boot']+self.files[1]['BOOT-LICENSES.txt']+self.artifacts[1][:release.PREFIX]+bundle.blake(self.artifacts[1]))

    def test_embedding_rejects_wrong_key_old_format_and_corruption_without_output(self):
        artifact=self.root/'input.release';output=self.root/'embedded.h';output.write_bytes(b'preserve')
        cases=[(self.artifacts[1],self.keys[0],'arm64'),(self.artifacts[0],self.keys[0],'arm64'),
               (self.artifacts[1],self.keys[1],'armv7'),(self.artifacts[1][:-1],self.keys[1],'arm64')]
        changed=bytearray(self.artifacts[1]);changed[-1]^=1;cases.append((changed,self.keys[1],'arm64'))
        for data,key,arch in cases:
            artifact.write_bytes(data)
            with self.assertRaises(ValueError):management_blob.emit(artifact,key,arch,output,VERIFIER)
            self.assertEqual(output.read_bytes(),b'preserve')
        self.assertFalse(any(p.name.startswith('pd-management-verify-') for p in self.root.iterdir()))

    def test_cache_and_unit_directory_symlinks_or_writable_modes_refuse(self):
        for p in [self.store,self.store/'units',self.units]:
            p.chmod(0o777);self.invoke(expected=1);p.chmod(0o700)
        p=self.store/'units';p.rename(self.store/'elsewhere');p.symlink_to(self.store/'elsewhere');self.invoke(expected=1)

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--checker',type=Path,required=True);p.add_argument('--verifier',type=Path,required=True);a,rest=p.parse_known_args();CHECKER=a.checker.resolve();VERIFIER=a.verifier.resolve();unittest.main(argv=[__file__,*rest])
