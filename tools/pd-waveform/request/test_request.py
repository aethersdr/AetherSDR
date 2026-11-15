#!/usr/bin/env python3
"""Offline waveform-to-carrier contract using actual native entry and client."""
import argparse
import array
import concurrent.futures
import fcntl
import hashlib
import io
import json
import os
from pathlib import Path
import shutil
import socket
import stat
import struct
import subprocess
import tempfile
import unittest
import zipfile
import client
import package

REQUEST=None
PRODUCTION=None

class RequestTest(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(prefix='pd-native-request-');self.addCleanup(self.temp.cleanup)
        self.path=Path(self.temp.name);shutil.copy2(REQUEST,self.path/'pd-request')
        self.key=bytes(range(32));self.inputs=[os.urandom(185),b'p'*352,b'c'*432,b'o'*432]
        self.receipt=b'PDDELIV1'+struct.pack('>II',64,2)+client.digest(self.inputs[0])+client.digest(self.inputs[1])
        self.config=b'PDXFER01'+self.key+self.receipt
        (self.path/'config').write_bytes(self.config);(self.path/'config').chmod(0o600)
        self.executor=concurrent.futures.ThreadPoolExecutor(max_workers=1);self.addCleanup(self.executor.shutdown,wait=True)
    def start(self,arguments=()):
        listener=socket.socket();listener.bind(('127.0.0.1',0));listener.listen(1);self.addCleanup(listener.close)
        p=subprocess.Popen(['setpriv','--inh-caps=-all',str(self.path/'pd-request'),*arguments],pass_fds=[listener.fileno()],env=dict(os.environ,PD_TEST_LISTENER=str(listener.fileno())),stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
        self.addCleanup(self.reap,p)
        address=listener.getsockname();listener.close()
        return p,address
    def reap(self,p):
        if p.poll() is None:p.kill()
        p.communicate(timeout=5)
    def carrier(self,result=0,wrong_key=False,close=False):
        listener=socket.socket(socket.AF_UNIX,socket.SOCK_SEQPACKET);listener.settimeout(4)
        listener.bind('\0aetherpd-bootstrap-'+self.receipt[16:48].hex());listener.listen(1)
        def serve():
            files=[]
            try:
                with listener.accept()[0] as peer:
                    peer.settimeout(3);nonce=os.urandom(32)
                    def mac(domain):return hashlib.blake2b(domain+nonce+self.receipt,key=b'x'*32 if wrong_key else self.key,digest_size=32).digest()
                    peer.sendall(b'PDCARR01'+nonce+mac(b'PDSERV01'))
                    data,control,flags,_=peer.recvmsg(64,socket.CMSG_SPACE(4*8))
                    for level,kind,payload in control:
                        if level==socket.SOL_SOCKET and kind==socket.SCM_RIGHTS:
                            values=array.array('i');values.frombytes(payload);files.extend(values)
                    if wrong_key:
                        self.assertEqual(files,[]);self.assertEqual(data,b'');return {'descriptors_received':False}
                    self.assertEqual(flags,0);self.assertEqual(data,b'PDROOT01'+mac(b'PDSEND01'));self.assertEqual(len(files),5)
                    self.assertTrue(stat.S_ISDIR(os.fstat(files[0]).st_mode));self.assertEqual(os.fstat(files[0]).st_uid,0)
                    for fd,expected in zip(files[1:],self.inputs):
                        self.assertEqual(os.fstat(fd).st_nlink,0);self.assertEqual(fcntl.fcntl(fd,fcntl.F_GETFL)&os.O_ACCMODE,os.O_RDONLY)
                        self.assertEqual(os.pread(fd,len(expected)+1,0),expected)
                    if not close:peer.sendall(b'PDDONE01'+struct.pack('>I',result))
                    return {'descriptors_received':True}
            finally:
                for fd in files:os.close(fd)
                listener.close()
        return self.executor.submit(serve)
    def result(self,p,expected):
        stdout,stderr=p.communicate(timeout=5);self.assertEqual(stderr,'');self.assertEqual(p.returncode,expected,stdout)
        if stdout:self.assertFalse(json.loads(stdout)['installation_committed'])
    def test_complete_transfer_and_delivery_result_are_not_commit(self):
        p,address=self.start()
        with socket.create_connection(address,2) as channel:
            upload=client.Upload(channel,self.config);stage=upload.send(self.inputs)
            self.assertEqual(stage,{'inputs_verified':True,'installation_committed':False})
            future=self.carrier();self.assertEqual(upload.finish(),{'delivery_result':0,'installation_committed':False});self.assertTrue(future.result(timeout=5)['descriptors_received'])
            with self.assertRaises(ValueError):upload.finish()
        self.result(p,0)
    def test_installer_refusal_failure_cleanup_and_disconnect(self):
        for result in [3,4,5,None,99]:
            with self.subTest(result=result):
                p,address=self.start()
                with socket.create_connection(address,2) as channel:
                    upload=client.Upload(channel,self.config);upload.send(self.inputs)
                    future=self.carrier(result=0 if result is None else result,close=result is None)
                    expected=result if result in [3,4,5] else 6
                    self.assertEqual(upload.finish()['delivery_result'],expected);future.result(timeout=5)
                self.result(p,expected)
    def test_wrong_carrier_authentication_gets_no_host_descriptor(self):
        p,address=self.start()
        with socket.create_connection(address,2) as channel:
            upload=client.Upload(channel,self.config);upload.send(self.inputs);future=self.carrier(wrong_key=True)
            self.assertEqual(upload.finish()['delivery_result'],6);self.assertFalse(future.result(timeout=5)['descriptors_received'])
        self.result(p,6)
    def test_wrong_desktop_key_refuses_endpoint(self):
        p,address=self.start()
        with socket.create_connection(address,2) as channel:
            upload=client.Upload(channel,b'PDXFER01'+b'x'*32+self.receipt)
            with self.assertRaisesRegex(ValueError,'endpoint authentication'):upload.send(self.inputs)
        self.result(p,6)
    def test_configuration_symlink_permissions_and_arguments_refused(self):
        (self.path/'config').chmod(0o644);p,_=self.start();self.result(p,3)
        (self.path/'config').chmod(0o600);(self.path/'config').rename(self.path/'other');(self.path/'config').symlink_to('other');p,_=self.start();self.result(p,3)
        p,_=self.start(['arbitrary-command']);self.result(p,2)
    def test_production_entry_refuses_local_user(self):
        if os.getuid()==78666:self.skipTest('test user equals production waveform user')
        p=subprocess.run([str(PRODUCTION)],capture_output=True,text=True,timeout=3);self.assertEqual(p.returncode,2)
    def test_client_validates_inputs_before_sending(self):
        a,b=socket.socketpair();self.addCleanup(a.close);self.addCleanup(b.close);a.settimeout(.05);b.settimeout(.05)
        upload=client.Upload(a,self.config)
        with self.assertRaises(ValueError):upload.send([*self.inputs[:3],b''])
        with self.assertRaises(socket.timeout):b.recv(1)
        with self.assertRaises(ValueError):upload.finish()
    def test_authenticated_acknowledgement_binding(self):
        a,b=socket.socketpair();self.addCleanup(a.close);self.addCleanup(b.close)
        upload=client.Upload(a,self.config);upload.challenge=b'n'*32
        prefix=b'PDUPACK1'+struct.pack('>II',2,0)
        b.sendall(prefix+bytes(32))
        import time
        with self.assertRaisesRegex(ValueError,'authenticated'):upload.acknowledgement(2,time.monotonic()+1)
        b.sendall(prefix+upload.mac(b'PDUACK01',prefix))
        self.assertEqual(upload.acknowledgement(2,time.monotonic()+1),0)
    def test_private_package_reproducible_and_contains_no_release_payloads(self):
        executable=bytearray(64);executable[:6]=b'\x7fELF\x02\x01';struct.pack_into('<H',executable,18,183)
        first,info=package.assemble(bytes(executable),self.config,b'licenses');second,_=package.assemble(bytes(executable),self.config,b'licenses');self.assertEqual(first,second)
        with zipfile.ZipFile(io.BytesIO(first)) as archive:
            self.assertEqual(set(archive.namelist()),{'AetherPDInstall/'+n for n in ['AetherPDInstall.cfg','pd-request','config','LICENSES']})
            self.assertEqual(archive.read('AetherPDInstall/config'),self.config)
            self.assertTrue(all((x.external_attr>>16)&0o077==0 for x in archive.infolist()))
        self.assertFalse(info['contains_release_payloads']);target=self.path/'AetherPDInstall.ssdr_waveform';package.publish(target,first)
        self.assertEqual(stat.S_IMODE(target.stat().st_mode),0o600)
        with self.assertRaises(FileExistsError):package.publish(target,first)
        struct.pack_into('<H',executable,18,40)
        with self.assertRaises(ValueError):package.assemble(bytes(executable),self.config,b'licenses')

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--request',type=Path,required=True);parser.add_argument('--production',type=Path,required=True);args,remaining=parser.parse_known_args();REQUEST=args.request.resolve();PRODUCTION=args.production.resolve();unittest.main(argv=[__file__,*remaining])
