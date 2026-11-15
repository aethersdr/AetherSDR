"""Bind an authenticated operator decision to exact artifacts; grants no root.

The delivery adapter must create the resulting record as root-owned mode0600
only after its operator consent flow. Possession of these bytes is not consent.
"""
import struct
import approval
import bootstrap_artifact
import bundle
import migration
import upgrade


def target(raw):
    if len(raw)!=140 or raw[:8]!=b'PDAPRV02':
        raise ValueError('invalid target record')
    profile=struct.unpack_from('>I',raw,8)[0]
    firmware=raw[48:76].split(b'\0',1)[0].decode('ascii')
    serial=raw[76:140].split(b'\0',1)[0].decode('ascii')
    if raw!=approval.encode(profile,firmware,serial,raw[16:48]):
        raise ValueError('noncanonical target')
    return profile,firmware,serial,raw[16:48]


def encode(envelope, plan):
    if len(envelope)<bootstrap_artifact.PREFIX or envelope[:8]!=b'AEPDX001':
        raise ValueError('invalid envelope framing')
    arch,role,size,reserved=struct.unpack_from('>IIII',envelope,8)
    if (arch not in (7,64) or role not in (1,2) or reserved or not 0<size<=bootstrap_artifact.MAX_WORKER
            or len(envelope)!=bootstrap_artifact.PREFIX+size):
        raise ValueError('invalid envelope shape')
    if role==1:
        if arch!=64 or len(plan)!=308 or plan[:8]!=b'PDMIG001':
            raise ValueError('unsupported installation plan')
        candidate=target(plan[8:148])
        if plan!=migration.encode(plan[8:148],plan[148:212].decode('ascii'),plan[212:276].decode('ascii'),plan[276:]):
            raise ValueError('invalid installation plan')
    else:
        if len(plan)!=352 or plan[:8] not in (b'PDUPGR01', b'PDUPGR02'):
            raise ValueError('invalid upgrade plan')
        attempt=plan[56:88] if plan[:8]==b'PDUPGR02' else None
        previous=target(plan[8:56]+plan[196:288] if attempt is not None else plan[8:148])
        candidate=target(plan[148:288])
        if previous[:3]!=candidate[:3] or plan!=upgrade.encode(*candidate[:3],previous[3],candidate[3],plan[288:320],plan[320:352],attempt=attempt):
            raise ValueError('invalid upgrade transition')
    if candidate[0]!=(1 if arch==64 else 2) or candidate[3]!=envelope[56:88]:
        raise ValueError('envelope and plan target different releases')
    return b'PDDELIV1'+struct.pack('>II',arch,role)+bundle.blake(envelope)+bundle.blake(plan)
