"""Authenticated input upload over an existing connection; never asserts commit.

The caller authenticates distributed artifacts and obtains exact-plan consent
before producing the private configuration or using this transfer. This module
has no radio discovery, installation, keying or generic command interface.
"""
import hashlib
import hmac
import socket
import struct
import time

MAX_RELEASE=18*1024*1024

def digest(data):return hashlib.blake2b(data,digest_size=32).digest()

def configuration(data):
    if (len(data)!=120 or data[:8]!=b'PDXFER01' or not any(data[8:40])
            or data[40:48]!=b'PDDELIV1'):
        raise ValueError('invalid private request configuration')
    arch,role=struct.unpack_from('>II',data,48)
    if arch not in (7,64) or role not in (1,2) or (role==1 and arch!=64):
        raise ValueError('unsupported request role')
    return data[8:40],data[40:]

def public_record(data):
    _,receipt=configuration(data)
    if struct.unpack_from('>I',receipt,12)[0]!=2:
        raise ValueError('RAM credential provisioning requires an installed-radio upgrade')
    return b'PDPUB001'+receipt+digest(b'PDKEY001'+data)


def header(receipt,inputs):
    if len(inputs)!=4:raise ValueError('fixed input roles required')
    role=struct.unpack_from('>I',receipt,12)[0]
    sizes=list(map(len,inputs))
    if (not 184<sizes[0]<=184+512*1024 or sizes[1]!=(308 if role==1 else 352)
            or not 432<=sizes[2]<=MAX_RELEASE or (not 432<=sizes[3]<=MAX_RELEASE if role==2 else sizes[3]!=0)
            or digest(inputs[0])!=receipt[16:48] or digest(inputs[1])!=receipt[48:80]):
        raise ValueError('inputs do not match the approved request')
    return b'PDUPLD01'+struct.pack('>4I',*sizes)+b''.join(digest(data) if data else bytes(32) for data in inputs)

class Upload:
    def __init__(self,channel,private_config):
        self.channel=channel
        self.key,self.receipt=configuration(private_config)
        self.challenge=None
        self.inputs_verified=False
        self.finished=False
    def mac(self,domain,extra=b''):
        return hashlib.blake2b(domain+self.challenge+self.receipt+extra,key=self.key,digest_size=32).digest()
    def exact(self,size,deadline):
        data=bytearray()
        while len(data)<size:
            remaining=deadline-time.monotonic()
            if remaining<=0:raise TimeoutError('installer response deadline')
            self.channel.settimeout(remaining)
            chunk=self.channel.recv(size-len(data))
            if not chunk:raise EOFError('installer closed before authenticated result')
            data.extend(chunk)
        return bytes(data)
    def acknowledgement(self,stage,deadline):
        data=self.exact(48,deadline)
        if (data[:8]!=b'PDUPACK1' or struct.unpack_from('>I',data,8)[0]!=stage
                or not hmac.compare_digest(data[16:],self.mac(b'PDUACK01',data[:16]))):
            raise ValueError('invalid authenticated installer acknowledgement')
        result=struct.unpack_from('>I',data,12)[0]
        if result not in (0,3,4,5,6) or (stage==1 and result):
            raise ValueError('invalid installer stage result')
        return result
    def send(self,inputs):
        if self.challenge is not None:raise ValueError('request already attempted')
        frame=header(self.receipt,inputs)
        deadline=time.monotonic()+90
        hello=self.exact(72,deadline)
        self.challenge=hello[8:40]
        if hello[:8]!=b'PDUPCH01' or not hmac.compare_digest(hello[40:],self.mac(b'PDUSRV01')):
            raise ValueError('installer endpoint authentication failed')
        for data in [frame+self.mac(b'PDUCLI01',frame),*inputs]:
            view=memoryview(data)
            while view:
                remaining=deadline-time.monotonic()
                if remaining<=0:raise TimeoutError('installer input deadline')
                self.channel.settimeout(remaining)
                amount=self.channel.send(view[:65536])
                if amount<=0:raise EOFError('installer closed during input')
                view=view[amount:]
        self.channel.shutdown(socket.SHUT_WR)
        self.acknowledgement(1,deadline)
        self.inputs_verified=True
        return {'inputs_verified':True,'installation_committed':False}
    def finish(self):
        if not self.inputs_verified or self.finished:raise ValueError('request is not awaiting completion')
        self.finished=True
        result=self.acknowledgement(2,time.monotonic()+190)
        return {'delivery_result':result,'installation_committed':False}
