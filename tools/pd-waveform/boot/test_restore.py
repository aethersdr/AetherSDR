#!/usr/bin/env python3
"""Native fixed adapters with real filesystem journals and a test Podman model."""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
import approval
import migration
from test_predecessor import encoded, inventory

CHECKER=None

class RestoreTest(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(prefix='pd-restore-');self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name);self.store=self.root/'var/lib/smartsdr/aetherpd';self.store.mkdir(parents=True,mode=0o700)
        self.image_layer=self.root/('var/lib/smartsdr/containers/storage/overlay/'+'d'*64+'/diff')
        self.merged=self.root/('var/lib/smartsdr/containers/storage/overlay/'+'e'*64+'/merged')
        self.upper=self.merged.parent/'diff'
        self.binary=b'approved static executable fixture'
        for p in [self.image_layer,self.merged,self.upper]:
            p.mkdir(parents=True);(p/'flex-tailnet-shim').write_bytes(self.binary);(p/'flex-tailnet-shim').chmod(0o755)
        self.target=approval.encode(1,'4.3.16.42034','fixture-8600',bytes(range(32)))
        self.approval=migration.encode(self.target,'a'*64,'b'*64,hashlib.blake2b(self.binary,digest_size=32).digest())
        self.write(self.root/'approval',self.approval)
        self.recipe=b'PDREC001'+self.approval+b'192.168.50.100'.ljust(16,b'\0')
        self.write(self.store/'recovery-recipe',self.recipe);self.write(self.store/'migration.lock',b'')
        self.phase(6);self.model(0,0,0,0,0)
        self.hostname='aetherpd-'+hashlib.blake2b(self.recipe,digest_size=32).hexdigest()[:32]
        self.config=b'current provision';self.identity=b'current primary identity'
        state=self.store/'state';state.mkdir(mode=0o700);(state/'tsnet').mkdir(mode=0o700)
        self.write(state/'provision.json',self.config);self.write(state/'tsnet/tailscaled.state',self.identity)
        snapshot=b'PDSTATE1'+self.target+b''.join(struct.pack('>I',len(b))+hashlib.blake2b(b,digest_size=32).digest() for b in [self.config,self.identity])
        self.write(self.store/'identity-transferred',snapshot)
        image=['PDIMG001','b'*64,'arm64','linux','layers','1','sha256:'+'d'*64,'overlay','2',
               '/var/lib/smartsdr/containers/storage/overlay/'+'d'*64+'/diff',
               '/var/lib/smartsdr/containers/storage/overlay/'+'d'*64+'/work',
               json.dumps({'Entrypoint':['/flex-tailnet-shim'],'Labels':{'com.flexradio.waveform.name':'flex-tailnet-shim','com.flexradio.waveform.version':'0.5.2'}},separators=(',',':'))]
        self.write(self.root/'image-inventory',('\n'.join(image)+'\n').encode())
        for role,cid in [('original','a'*64),('restored','c'*64)]:
            overlay=['PDOVL001',cid,'b'*64,'overlay','3','',
                     '/var/lib/smartsdr/containers/storage/overlay/'+'e'*64+'/diff',
                     '/var/lib/smartsdr/containers/storage/overlay/'+'e'*64+'/work',
                     '/var/lib/smartsdr/containers/storage/overlay/'+'d'*64+'/diff']
            self.write(self.root/('overlay-'+role),('\n'.join(overlay)+'\n').encode())
        for restored in [False,True]:
            for running in [False,True]:
                fields=inventory();fields[33]=encoded('SSDR_RADIO_ADDRESS=192.168.50.100')
                fields[25]='true' if running else 'false';fields[26]='1234' if running else '0'
                if restored:
                    fields[0]='PDREST01';fields[1]='c'*64;fields[22]='RESTORED'
                    fields[16]='["/var/lib/smartsdr/aetherpd/legacy/state:/var/lib/flex-tailnet-shim:rw,rprivate,nosuid,rbind"]'
                    fields[32]=encoded('HOSTNAME='+self.hostname)
                    fields += [encoded(self.hostname),'1',encoded('bind'),encoded('/var/lib/smartsdr/aetherpd/legacy/state'),
                               encoded('/var/lib/flex-tailnet-shim'),'','','["nosuid","rbind"]','true',encoded('rprivate')]
                self.write(self.root/(('restored' if restored else 'original')+('-running' if running else '-stopped')),('\n'.join(fields)+'\n').encode())

    def write(self,path,data):
        path.write_bytes(data);path.chmod(0o600)

    def model(self,*values):self.write(self.root/'model',struct.pack('<5I',*values))
    def current(self):return struct.unpack('<5I',(self.root/'model').read_bytes())
    def phase(self,value):self.write(self.store/'transaction',b'PDTXN001'+self.approval+struct.pack('>I',value))
    def commands(self):
        return [[x.decode() for x in part.split(b'\0')] for part in (self.root/'trace').read_bytes().split(b'\0\0') if part]
    def invoke(self,action='restore',fault='',expected=0):
        p=subprocess.run([str(CHECKER),action,str(self.root),fault],capture_output=True,timeout=10)
        error=p.stderr.decode(errors='replace')
        self.assertNotIn('Sanitizer',error);self.assertNotIn('runtime error:',error)
        self.assertEqual(p.returncode,expected,error)
        return self.commands()
    def no_mutations(self,commands):
        self.assertFalse(any(c[1] in ['create','rm','start','mount','unmount'] for c in commands),commands)
    def alter(self,file,index,value):
        p=self.root/file;lines=p.read_text().splitlines();lines[index]=value;p.write_text('\n'.join(lines)+'\n')

    def test_restore_prepares_state_before_fixed_create(self):
        commands=self.invoke();self.assertEqual(self.current(),(0,0,1,1,0))
        state=self.store/'legacy/state';self.assertEqual((state/'provision.json').read_bytes(),self.config)
        self.assertEqual((state/'tsnet/tailscaled.state').read_bytes(),self.identity)
        create=next(c for c in commands if c[1]=='create')
        self.assertEqual(create,['/usr/bin/podman','create','--pull=never','--name','flex-tailnet-shim','--hostname',self.hostname,
            '--network=host','--pid=private','--ipc=shareable','--restart=always','--cap-drop=AUDIT_WRITE',
            '--cap-drop=MKNOD','--cap-drop=NET_RAW','--workdir=/','--volume',
            '/var/lib/smartsdr/aetherpd/legacy/state:/var/lib/flex-tailnet-shim:rw,rprivate,nosuid,rbind',
            '--env','PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin','--env','TERM=xterm',
            '--env','container=podman','--env','SSDR_RADIO_ADDRESS=192.168.50.100','b'*64])
        self.assertEqual([c for c in commands if c[1]=='start'],[['/usr/bin/podman','start','c'*64]])

    def test_restore_retry_does_not_restart_or_overwrite_latest_identity(self):
        self.invoke();legacy=self.store/'legacy/state/tsnet/tailscaled.state'
        self.write(legacy,b'new legacy identity');self.write(self.store/'state/tsnet/tailscaled.state',b'stale primary identity')
        commands=self.invoke();self.no_mutations(commands);self.assertEqual(legacy.read_bytes(),b'new legacy identity')

    def test_create_process_death_reconciles_existing_owned_container(self):
        self.invoke(fault='kill:create',expected=-9);self.assertEqual(self.current(),(0,0,1,0,0))
        commands=self.invoke();self.assertFalse(any(c[1]=='create' for c in commands));self.assertEqual(self.current(),(0,0,1,1,0))

    def test_create_timeout_reconciles_actual_result(self):
        self.invoke(fault='create-timeout',expected=1);self.assertEqual(self.current(),(0,0,1,0,0))
        self.invoke();self.assertEqual(self.current(),(0,0,1,1,0))

    def test_start_process_death_verifies_without_restart(self):
        self.invoke(fault='kill:start',expected=-9);commands=self.invoke();self.no_mutations(commands)

    def test_existing_mount_references_are_not_touched(self):
        self.model(0,0,0,0,2);commands=self.invoke();self.assertEqual(self.current()[4],2)
        self.assertFalse(any(c[1] in ['mount','unmount'] for c in commands))

    def test_original_present_refuses_restore(self):
        for running in [0,1]:
            self.model(1,running,0,0,0);self.no_mutations(self.invoke(expected=1))

    def test_foreign_named_container_preserved(self):
        self.model(0,0,1,0,0);self.alter('restored-stopped',35,encoded('foreign-host'))
        self.no_mutations(self.invoke(expected=1));self.assertEqual(self.current(),(0,0,1,0,0))
        self.assertFalse((self.store/'legacy').exists())

    def test_running_foreign_executable_never_restarted(self):
        self.invoke();(self.merged/'flex-tailnet-shim').write_bytes(b'foreign')
        self.no_mutations(self.invoke(expected=1));self.assertEqual(self.current(),(0,0,1,1,0))

    def test_wrong_stopped_executable_is_not_started(self):
        (self.upper/'flex-tailnet-shim').write_bytes(b'foreign')
        commands=self.invoke(expected=1);self.assertFalse(any(c[1]=='start' for c in commands))
        self.assertEqual(self.current(),(0,0,1,0,0))

    def test_wrong_image_executable_prevents_create(self):
        (self.image_layer/'flex-tailnet-shim').write_bytes(b'foreign')
        self.no_mutations(self.invoke(expected=1));self.assertFalse((self.store/'legacy').exists())

    def test_image_verification_is_read_only(self):
        commands=self.invoke('image');self.assertTrue(all(c[1:3]==['image','inspect'] for c in commands))
        self.assertEqual(len(commands),2)

    def test_unsafe_image_paths_refused(self):
        for index,value in [(1,'c'*64),(2,'arm'),(3,'other'),(4,'other'),(5,'2'),(6,'sha256:'+'D'*64),
                            (7,'vfs'),(8,'3'),(9,'/tmp/diff'),(10,'/tmp/work'),(11,'{}')]:
            with self.subTest(index=index):
                p=self.root/'image-inventory';before=p.read_bytes();self.alter('image-inventory',index,value)
                self.no_mutations(self.invoke(expected=1));p.write_bytes(before)

    def test_image_symlink_and_writable_binary_refused(self):
        binary=self.image_layer/'flex-tailnet-shim';binary.chmod(0o777)
        self.no_mutations(self.invoke(expected=1));binary.unlink();binary.symlink_to(self.merged/'flex-tailnet-shim')
        self.no_mutations(self.invoke(expected=1))

    def test_image_hardlink_and_unsafe_parent_refused(self):
        binary=self.image_layer/'flex-tailnet-shim';os.link(binary,self.root/'alias')
        self.no_mutations(self.invoke(expected=1));(self.root/'alias').unlink()
        self.image_layer.chmod(0o777);self.no_mutations(self.invoke(expected=1));self.image_layer.chmod(0o755)

    def test_phase_and_approval_fence(self):
        for phase in [1,2,3,4,5,7]:
            self.phase(phase);self.no_mutations(self.invoke(expected=1))
        self.phase(6);p=self.store/'transaction';data=bytearray(p.read_bytes());data[20]^=1;p.write_bytes(data)
        self.no_mutations(self.invoke(expected=1))

    def test_runtime_fence(self):
        runtime=self.root/'run/aetherpd';runtime.mkdir(parents=True)
        self.no_mutations(self.invoke(expected=1));runtime.rmdir()
        commands=self.invoke(fault='runtime-after-create',expected=1)
        self.assertFalse(any(c[1]=='start' for c in commands));self.assertEqual(self.current(),(0,0,1,0,0))

    def test_missing_or_changed_recipe_refused(self):
        p=self.store/'recovery-recipe';before=p.read_bytes();p.unlink();self.no_mutations(self.invoke(expected=1))
        self.write(p,before[:-1]);self.no_mutations(self.invoke(expected=1))

    def test_unsafe_transaction_and_lock_refused(self):
        p=self.store/'transaction';p.chmod(0o644);self.no_mutations(self.invoke(expected=1));p.chmod(0o600)
        p=self.store/'migration.lock';p.chmod(0o644);self.no_mutations(self.invoke(expected=1));p.chmod(0o600)
        fd=os.open(p,os.O_RDWR)
        try:
            fcntl.flock(fd,fcntl.LOCK_EX|fcntl.LOCK_NB);self.no_mutations(self.invoke(expected=1))
        finally:os.close(fd)

    def test_missing_identity_refuses_creation(self):
        (self.store/'state/tsnet/tailscaled.state').unlink()
        self.no_mutations(self.invoke(expected=1));self.assertEqual(self.current(),(0,0,0,0,0))

    def test_command_errors_are_not_absence(self):
        for fault in ['image-error','exists-error']:
            self.no_mutations(self.invoke(fault=fault,expected=1))

    def test_create_and_start_failures_preserve_retry(self):
        self.invoke(fault='create-fail',expected=1);self.assertEqual(self.current(),(0,0,0,0,0))
        self.invoke(fault='start-fail',expected=1);self.assertEqual(self.current(),(0,0,1,0,0))
        self.invoke();self.assertEqual(self.current(),(0,0,1,1,0))

    def test_overlay_upper_metadata_is_not_redirected(self):
        os.setxattr(self.upper,'user.overlay.redirect',b'/other')
        commands=self.invoke(expected=1);self.assertFalse(any(c[1]=='start' for c in commands))
        os.removexattr(self.upper,'user.overlay.redirect');os.setxattr(self.upper,'user.overlay.opaque',b'y')
        self.no_mutations(self.invoke(expected=1))

    def test_overlay_whiteout_is_refused(self):
        (self.upper/'.wh.flex-tailnet-shim').write_bytes(b'')
        commands=self.invoke(expected=1);self.assertFalse(any(c[1]=='start' for c in commands))

    def test_overlay_requires_exact_container_and_one_approved_lower(self):
        self.model(0,0,1,0,0)
        for index,value in [(1,'a'*64),(2,'f'*64),(3,'vfs'),(4,'5'),(5,'/tmp/merged'),
                            (6,'/tmp/diff'),(7,'/tmp/work'),(8,'/tmp/diff')]:
            with self.subTest(index=index):
                p=self.root/'overlay-restored';before=p.read_bytes();self.alter('overlay-restored',index,value)
                self.no_mutations(self.invoke(expected=1));p.write_bytes(before)

    def test_missing_upper_executable_uses_verified_image(self):
        (self.upper/'flex-tailnet-shim').unlink();commands=self.invoke()
        self.assertFalse(any(c[1] in ['mount','unmount'] for c in commands))

    def test_mounted_overlay_has_a_matching_merged_directory(self):
        self.alter('overlay-restored',4,'4')
        self.alter('overlay-restored',5,'/var/lib/smartsdr/containers/storage/overlay/'+'e'*64+'/merged')
        self.invoke()

    def test_merged_directory_presence_matches_graph_field_count(self):
        self.model(0,0,1,0,0)
        self.alter('overlay-restored',4,'4');self.no_mutations(self.invoke(expected=1))
        self.alter('overlay-restored',4,'3');self.alter('overlay-restored',5,'/var/lib/smartsdr/containers/storage/overlay/'+'e'*64+'/merged')
        self.no_mutations(self.invoke(expected=1))

    def test_retire_removes_only_approved_stopped_id(self):
        self.model(1,0,0,0,0);self.phase(3);commands=self.invoke('retire')
        self.assertEqual(self.current(),(0,0,0,0,0));self.assertEqual([c for c in commands if c[1]=='rm'],[['/usr/bin/podman','rm','a'*64]])
        self.assertFalse(any(c[1] in ['rmi','start','create'] for c in commands))
        self.no_mutations(self.invoke('retire'))

    def test_retire_requires_copy_and_stopped_predecessor(self):
        self.phase(3);self.model(1,1,0,0,0);self.no_mutations(self.invoke('retire',expected=1))
        self.model(1,0,0,0,0);(self.store/'identity-transferred').unlink();self.no_mutations(self.invoke('retire',expected=1))
        self.assertEqual(self.current(),(1,0,0,0,0))

    def test_death_after_retirement_can_reconstruct(self):
        self.model(1,0,0,0,0);self.phase(3);self.invoke('retire','kill:rm',-9)
        self.assertEqual(self.current(),(0,0,0,0,0));self.phase(6);self.invoke()
        self.assertEqual(self.current(),(0,0,1,1,0))

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--checker',type=Path,required=True)
    args,rest=parser.parse_known_args();CHECKER=args.checker.resolve()
    unittest.main(argv=['test_restore.py',*rest])
