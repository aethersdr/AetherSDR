#!/usr/bin/env python3
"""Real Unix descriptor transfer for our fixed bootstrap protocol; no radio."""
import argparse
import array
import hashlib
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import unittest

CHECKER=None
class TransportTest(unittest.TestCase):
    def setUp(self):
        self.fds=[];self.addCleanup(self.close_files)
        self.key=bytes(range(32));self.receipt=b'PDDELIV1'+struct.pack('>II',64,2)+bytes(range(64))
        self.config=self.file(self.key+self.receipt)
        root=os.open('/',os.O_RDONLY|os.O_DIRECTORY);self.fds.append(root)
        self.inputs=[root,self.file(b'e'*185),self.file(b'p'*352),self.file(b'c'*432),self.file(b'o'*432)]
    def file(self,data,readonly=True):
        fd=os.memfd_create('pd-transport-test',os.MFD_CLOEXEC);self.fds.append(fd);os.write(fd,data);os.fchmod(fd,0o600)
        if readonly:
            fd=os.open('/proc/self/fd/'+str(fd),os.O_RDONLY|os.O_CLOEXEC);self.fds.append(fd)
        return fd
    def close_files(self):
        for fd in self.fds:os.close(fd)
    def pair(self,kind=socket.SOCK_SEQPACKET):
        a,b=socket.socketpair(socket.AF_UNIX,kind);a.settimeout(5);b.settimeout(5)
        self.addCleanup(a.close);self.addCleanup(b.close);return a,b
    def start(self,mode,channel,key=None,receipt=None,uid=None,inputs=None):
        config=self.config if key is None and receipt is None else self.file((key or self.key)+(receipt or self.receipt))
        files=self.inputs if inputs is None else inputs
        p=subprocess.Popen([str(CHECKER),mode,str(config),str(channel.fileno()),str(os.getuid() if uid is None else uid)],
            pass_fds=[config,channel.fileno(),*files],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,
            env=dict(os.environ,PD_TEST_FDS=','.join(map(str,files))))
        self.addCleanup(self.reap,p);return p
    def reap(self,p):
        if p.poll() is None:p.kill()
        p.communicate(timeout=5)
    def result(self,p,expected):
        stdout,stderr=p.communicate(timeout=6);self.assertEqual(stderr,'');self.assertEqual(p.returncode,0 if expected else 3,stdout)
        result=json.loads(stdout);self.assertEqual(result['accepted'],expected)
        self.assertEqual(result['descriptors_before'],result['descriptors_after']);return result
    def mac(self,domain,challenge,key=None,receipt=None):
        return hashlib.blake2b(domain+challenge+(receipt or self.receipt),key=key or self.key,digest_size=32).digest()
    def hello(self,channel):
        hello=channel.recv(4096);self.assertEqual(len(hello),72);self.assertEqual(hello[:8],b'PDCARR01')
        self.assertEqual(hello[40:],self.mac(b'PDSERV01',hello[8:40]));return hello[8:40]
    def reply(self,channel,challenge,files=None,message=None):
        files=self.inputs if files is None else files
        channel.sendmsg([message or b'PDROOT01'+self.mac(b'PDSEND01',challenge)],
            [(socket.SOL_SOCKET,socket.SCM_RIGHTS,array.array('i',files))] if files else [])

    def test_both_native_ends_transfer_upgrade(self):
        a,b=self.pair();receiver=self.start('receive',a);sender=self.start('send',b)
        self.result(sender,True);self.result(receiver,True)
    def test_install_uses_four_descriptors(self):
        self.receipt=b'PDDELIV1'+struct.pack('>II',64,1)+bytes(range(64));self.config=self.file(self.key+self.receipt)
        self.inputs[2]=self.file(b'p'*308)
        a,b=self.pair();receiver=self.start('receive',a);sender=self.start('send',b)
        self.result(sender,True);self.result(receiver,True)
    def test_receiver_mac_and_cloexec_against_independent_encoder(self):
        a,b=self.pair();p=self.start('receive',a);challenge=self.hello(b);self.reply(b,challenge);self.result(p,True)
    def test_sender_mac_and_fds_against_independent_encoder(self):
        a,b=self.pair();p=self.start('send',a);challenge=bytes(reversed(range(32)))
        b.send(b'PDCARR01'+challenge+self.mac(b'PDSERV01',challenge))
        message,ancillary,flags,_=b.recvmsg(4096,socket.CMSG_SPACE(20))
        self.assertEqual(flags,0);self.assertEqual(message,b'PDROOT01'+self.mac(b'PDSEND01',challenge))
        received=[]
        for level,kind,payload in ancillary:
            self.assertEqual((level,kind),(socket.SOL_SOCKET,socket.SCM_RIGHTS));fds=array.array('i');fds.frombytes(payload);received.extend(fds)
        try:
            self.assertEqual(len(received),5)
            for original,copy in zip(self.inputs,received):self.assertEqual((os.fstat(original).st_dev,os.fstat(original).st_ino),(os.fstat(copy).st_dev,os.fstat(copy).st_ino))
        finally:
            for fd in received:os.close(fd)
        self.result(p,True)
    def test_wrong_peer_uid_and_wrong_socket_type_refuse(self):
        for mode in ['send','receive']:
            a,b=self.pair();self.result(self.start(mode,a,uid=os.getuid()+1),False)
            a,b=self.pair(socket.SOCK_STREAM);self.result(self.start(mode,a),False)
    def test_sender_refuses_wrong_server_key_or_receipt(self):
        for key,receipt in [(bytes(32),self.receipt),(self.key,self.receipt[:-1]+b'x')]:
            a,b=self.pair();p=self.start('send',a);challenge=bytes(32)
            b.send(b'PDCARR01'+challenge+self.mac(b'PDSERV01',challenge,key,receipt));self.result(p,False)
    def test_receiver_refuses_wrong_key_receipt_and_domain(self):
        for domain,key,receipt in [(b'PDSEND01',bytes(32),self.receipt),(b'PDSEND01',self.key,self.receipt[:-1]+b'x'),(b'PDSERV01',self.key,self.receipt)]:
            a,b=self.pair();p=self.start('receive',a);challenge=self.hello(b)
            self.reply(b,challenge,message=b'PDROOT01'+self.mac(domain,challenge,key,receipt));self.result(p,False)
    def test_replay_is_bound_to_fresh_server_challenge(self):
        a,b=self.pair();first=self.start('receive',a);challenge=self.hello(b);self.reply(b,challenge);self.result(first,True)
        a,b=self.pair();second=self.start('receive',a);fresh=self.hello(b);self.assertNotEqual(challenge,fresh)
        self.reply(b,challenge);self.result(second,False)
    def test_bad_packets_close_all_received_rights(self):
        for size in [0,39,41,4096]:
            a,b=self.pair();p=self.start('receive',a);challenge=self.hello(b)
            b.sendmsg([b'x'*size],[(socket.SOL_SOCKET,socket.SCM_RIGHTS,array.array('i',self.inputs))]);self.result(p,False)
        for count in [0,1,4,6,8,12]:
            a,b=self.pair();p=self.start('receive',a);challenge=self.hello(b)
            self.reply(b,challenge,files=[self.inputs[0]]*count);self.result(p,False)
    def test_sender_closes_unexpected_server_rights(self):
        a,b=self.pair();p=self.start('send',a);challenge=bytes(32)
        b.sendmsg([b'PDCARR01'+challenge+self.mac(b'PDSERV01',challenge)],[(socket.SOL_SOCKET,socket.SCM_RIGHTS,array.array('i',self.inputs))])
        self.result(p,False)
    def test_bad_descriptor_shapes_refuse_before_delivery(self):
        for index,bad in [(0,self.inputs[1]),(1,self.file(b'x'*184,False)),(2,self.file(b'x'*351)),(3,self.file(b'x'*431))]:
            files=list(self.inputs);files[index]=bad
            a,b=self.pair();p=self.start('receive',a);challenge=self.hello(b);self.reply(b,challenge,files=files);self.result(p,False)
            a,b=self.pair();self.result(self.start('send',a,inputs=files),False)
    def test_silent_peer_times_out(self):
        a,b=self.pair();p=self.start('receive',a);self.hello(b);self.result(p,False)

    def test_completion_codes_against_independent_encoder(self):
        for code in [0,3,4,5]:
            a,b=self.pair();p=self.start('result-send',a,uid=code)
            self.assertEqual(b.recv(128),b'PDDONE01'+struct.pack('>I',code));self.result(p,True)
            a,b=self.pair();p=self.start('result-receive',a)
            b.send(b'PDDONE01'+struct.pack('>I',code));self.result(p,True)

    def test_completion_refuses_malformed_and_unexpected_rights(self):
        for data in [b'',b'PDDONE01'+struct.pack('>I',1),b'PDDONE01'+struct.pack('>I',6),
                     b'PDDONE01'+struct.pack('>I',0x10003),b'PDDONE01'+bytes(3),b'PDDONE01'+bytes(5),b'XDDONE01'+bytes(4)]:
            a,b=self.pair();p=self.start('result-receive',a);b.send(data);self.result(p,False)
        a,b=self.pair();p=self.start('result-receive',a)
        b.sendmsg([b'PDDONE01'+bytes(4)],[(socket.SOL_SOCKET,socket.SCM_RIGHTS,array.array('i',self.inputs))]);self.result(p,False)
        a,b=self.pair();self.result(self.start('result-send',a,uid=2),False)

    def config_check(self,fd,expected,owner=None):
        p=subprocess.Popen([str(CHECKER),'config',str(fd),'0',str(os.getuid() if owner is None else owner)],
            pass_fds=[fd],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
        self.addCleanup(self.reap,p);self.result(p,expected)
    def test_private_configuration_shape_and_ownership(self):
        data=b'PDXFER01'+self.key+self.receipt;fd=self.file(data);self.config_check(fd,True)
        self.config_check(fd,False,owner=os.getuid()+1);os.fchmod(fd,0o644);self.config_check(fd,False)
        self.config_check(self.file(data,False),False)
        for bad in [data[:-1],data+b'x',b'x'+data[1:],b'PDXFER01'+bytes(32)+self.receipt,data[:48]+struct.pack('>I',99)+data[52:]]:
            self.config_check(self.file(bad),False)
    def test_linked_configuration_is_refused(self):
        with tempfile.TemporaryDirectory(prefix='pd-transport-config-') as directory:
            path=Path(directory)/'request';path.write_bytes(b'PDXFER01'+self.key+self.receipt);path.chmod(0o600)
            fd=os.open(path,os.O_RDONLY);self.fds.append(fd);self.config_check(fd,True)
            os.link(path,Path(directory)/'alias');self.config_check(fd,False)

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--checker',type=Path,required=True)
    args,rest=p.parse_known_args();CHECKER=args.checker.resolve();unittest.main(argv=[__file__,*rest])
