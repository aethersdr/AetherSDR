#!/usr/bin/env python3
"""Independent desktop-side wire encoder exercising native RAM-only reception."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import time
import unittest

CHECKER=None
DISK_DIRECTORY=None

def digest(data):return hashlib.blake2b(data,digest_size=32).digest()

def exact(channel,size):
    data=b''
    while len(data)<size:
        chunk=channel.recv(size-len(data))
        if not chunk:raise EOFError('closed during fixed frame')
        data+=chunk
    return data

class UploadTest(unittest.TestCase):
    def setUp(self):
        self.key=bytes(range(32));self.inputs=[b'e'*185,b'p'*352,b'c'*432,b'o'*432]
        self.receipt=b'PDDELIV1'+struct.pack('>II',64,2)+digest(self.inputs[0])+digest(self.inputs[1])
        self.directory=tempfile.TemporaryDirectory(prefix='pd-upload-test-',dir='/dev/shm');self.addCleanup(self.directory.cleanup)
        self.fds=[];self.addCleanup(self.close)
    def close(self):
        for fd in self.fds:os.close(fd)
    def start(self,receipt=None,key=None,directory=None,kind=socket.SOCK_STREAM):
        config=os.memfd_create('pd-upload-config',os.MFD_CLOEXEC);self.fds.append(config);os.write(config,(key if key is not None else self.key)+(receipt if receipt is not None else self.receipt))
        memory=os.open(directory or self.directory.name,os.O_RDONLY|os.O_DIRECTORY);self.fds.append(memory)
        if kind==socket.SOCK_STREAM:
            listener=socket.socket(socket.AF_INET,socket.SOCK_STREAM);listener.bind(('127.0.0.1',0));listener.listen(1)
            client=socket.create_connection(listener.getsockname(),2);server,_=listener.accept();listener.close()
        else:client,server=socket.socketpair(socket.AF_UNIX,kind)
        self.addCleanup(client.close);self.addCleanup(server.close);client.settimeout(3)
        p=subprocess.Popen([str(CHECKER),str(config),str(server.fileno()),str(memory)],pass_fds=[config,server.fileno(),memory],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
        server.close();self.addCleanup(self.reap,p);return p,client
    def reap(self,p):
        if p.poll() is None:p.kill()
        p.communicate(timeout=5)
    def hello(self,channel):
        data=exact(channel,72);self.assertEqual(data[:8],b'PDUPCH01');challenge=data[8:40]
        self.assertEqual(data[40:],self.mac(b'PDUSRV01',challenge));return challenge
    def mac(self,domain,challenge,header=b'',key=None,receipt=None):
        return hashlib.blake2b(domain+challenge+(receipt or self.receipt)+header,key=key or self.key,digest_size=32).digest()
    def header(self,inputs=None):
        values=self.inputs if inputs is None else inputs
        return b'PDUPLD01'+struct.pack('>4I',*[len(x) for x in values])+b''.join(digest(x) if x else bytes(32) for x in values)
    def send_header(self,channel,challenge,header=None,key=None,receipt=None):
        h=self.header() if header is None else header
        channel.sendall(h+self.mac(b'PDUCLI01',challenge,h,key,receipt))
    def body(self,channel,inputs=None):
        try:channel.sendall(b''.join(self.inputs if inputs is None else inputs));channel.shutdown(socket.SHUT_WR)
        except (BrokenPipeError,ConnectionResetError):pass
    def result(self,p,accepted,inputs=None):
        stdout,stderr=p.communicate(timeout=4);self.assertEqual(stderr,'');self.assertEqual(p.returncode,0 if accepted else 3,stdout)
        d=json.loads(stdout);self.assertEqual(d['accepted'],accepted);self.assertEqual(d['descriptors_before'],d['descriptors_after'])
        self.assertEqual(list(Path(self.directory.name).iterdir()),[])
        expected=[{'size':len(x),'hash':digest(x).hex()} for x in (inputs or self.inputs) if x] if accepted else []
        self.assertEqual(d['files'],expected)
        return d
    def test_upgrade_inputs_are_readonly_unlinked_and_exact(self):
        p,c=self.start();self.send_header(c,self.hello(c));c.sendall(b''.join(self.inputs));c.shutdown(socket.SHUT_WR);self.result(p,True)
    def test_install_inputs_without_predecessor(self):
        self.inputs=[b'e'*185,b'p'*308,b'c'*432,b''];self.receipt=b'PDDELIV1'+struct.pack('>II',64,1)+digest(self.inputs[0])+digest(self.inputs[1])
        p,c=self.start();self.send_header(c,self.hello(c));c.sendall(b''.join(self.inputs));c.shutdown(socket.SHUT_WR);self.result(p,True)
    def test_fragmented_frames_and_large_release(self):
        self.inputs[2]=b'c'*(4*1024*1024);p,c=self.start();nonce=self.hello(c);h=self.header();frame=h+self.mac(b'PDUCLI01',nonce,h)
        for byte in frame:c.sendall(bytes([byte]))
        c.sendall(b''.join(self.inputs));c.shutdown(socket.SHUT_WR);self.result(p,True)
    def test_wrong_key_refused_before_files(self):
        p,c=self.start();self.send_header(c,self.hello(c),key=b'k'*32);self.body(c);self.result(p,False)
    def test_header_mac_tampering_refused(self):
        p,c=self.start();nonce=self.hello(c);h=self.header();changed=bytearray(self.mac(b'PDUCLI01',nonce,h));changed[0]^=1
        c.sendall(h+changed);self.body(c);self.result(p,False)
    def test_replay_from_previous_challenge_refused(self):
        p,c=self.start();old=self.hello(c);c.shutdown(socket.SHUT_WR);self.result(p,False)
        p,c=self.start();new=self.hello(c);self.assertNotEqual(old,new);self.send_header(c,old);self.body(c);self.result(p,False)
    def test_receipt_binding_refuses_other_envelope_or_plan(self):
        for i in [0,1]:
            with self.subTest(input=i):
                p,c=self.start();values=list(self.inputs);values[i]=b'x'+values[i][1:];h=self.header(values);self.send_header(c,self.hello(c),h);self.body(c,values);self.result(p,False)
    def test_input_length_bounds_and_magic(self):
        cases=[(8,184),(8,184+512*1024+1),(12,351),(16,431),(16,18*1024*1024+1),(20,0),(20,18*1024*1024+1)]
        for offset,size in cases:
            with self.subTest(offset=offset,size=size):
                p,c=self.start();h=bytearray(self.header());struct.pack_into('>I',h,offset,size);self.send_header(c,self.hello(c),h);c.shutdown(socket.SHUT_WR);self.result(p,False)
        p,c=self.start();h=b'FOREIGN!'+self.header()[8:];self.send_header(c,self.hello(c),h);c.shutdown(socket.SHUT_WR);self.result(p,False)
    def test_each_content_hash_enforced(self):
        for i in range(4):
            with self.subTest(input=i):
                p,c=self.start();self.send_header(c,self.hello(c));parts=list(self.inputs);parts[i]=b'x'+parts[i][1:]
                try:c.sendall(b''.join(parts));c.shutdown(socket.SHUT_WR)
                except (BrokenPipeError,ConnectionResetError):pass
                self.result(p,False)
    def test_truncated_payload_and_trailing_bytes(self):
        for data in [b''.join(self.inputs)[:-1],b''.join(self.inputs)+b'x']:
            p,c=self.start();self.send_header(c,self.hello(c));c.sendall(data);c.shutdown(socket.SHUT_WR);self.result(p,False)
    def test_non_tmpfs_directory_refused(self):
        with tempfile.TemporaryDirectory(prefix='pd-upload-disk-',dir=DISK_DIRECTORY) as directory:
            p,c=self.start(directory=directory);self.result(p,False);self.assertEqual(list(Path(directory).iterdir()),[])
    def test_wrong_socket_type_and_zero_key_refused(self):
        p,c=self.start(kind=socket.SOCK_SEQPACKET);self.result(p,False)
        p,c=self.start(key=bytes(32));self.result(p,False)
    def test_silent_peer_and_missing_eof_deadlines(self):
        p,c=self.start();self.hello(c);start=time.monotonic();self.result(p,False);self.assertLess(time.monotonic()-start,2)
        p,c=self.start();self.send_header(c,self.hello(c));c.sendall(b''.join(self.inputs));self.result(p,False)
    def test_slow_progress_does_not_extend_total_deadline(self):
        p,c=self.start();self.send_header(c,self.hello(c));start=time.monotonic()
        while p.poll() is None and time.monotonic()-start<2:
            try:c.sendall(b'e')
            except (BrokenPipeError,ConnectionResetError):break
            time.sleep(.05)
        self.result(p,False);self.assertLess(time.monotonic()-start,2)
    def test_process_death_leaves_no_staging_paths(self):
        self.inputs[2]=b'c'*(4*1024*1024);p,c=self.start();self.send_header(c,self.hello(c));c.sendall(self.inputs[0]+self.inputs[1]+self.inputs[2][:65536]);time.sleep(.05)
        p.kill();p.communicate(timeout=3);self.assertEqual(list(Path(self.directory.name).iterdir()),[])

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--checker',type=Path,required=True);parser.add_argument('--disk-directory',type=Path,required=True);args,remaining=parser.parse_known_args();CHECKER=args.checker.resolve();DISK_DIRECTORY=args.disk_directory.resolve();unittest.main(argv=[__file__,*remaining])
