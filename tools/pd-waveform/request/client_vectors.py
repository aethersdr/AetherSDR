#!/usr/bin/env python3
"""Regenerate public dummy-key desktop uploader vectors; no credentials or radio."""
import json
from pathlib import Path
import struct
import client

ROOT = Path(__file__).resolve().parents[3]
DESTINATION = ROOT / 'tools/flex-tailnet-shim/testdata/aetherpd/upload.json'

def vectors():
    plan = bytearray.fromhex((DESTINATION.parent/'installation/approved.hex').read_text().strip())
    worker, candidate, previous = b'e'*185, b'c'*432, b'o'*433
    plan[24:56] = client.digest(previous)
    plan[164:196] = client.digest(candidate)
    inputs = [worker, bytes(plan), candidate, previous]
    receipt = b'PDDELIV1' + struct.pack('>II',64,2) + client.digest(worker) + client.digest(plan)
    private = b'PDXFER01' + bytes(range(32)) + receipt
    channel = client.Upload(None, private)
    channel.challenge = bytes(range(32,64))
    frame = client.header(receipt, inputs)
    result = {'note':'Public deterministic fixtures. The request key is not a credential.',
              'inputs':[v.hex() for v in inputs], 'private':private.hex(),
              'hello':(b'PDUPCH01'+channel.challenge+channel.mac(b'PDUSRV01')).hex(),
              'header':(frame+channel.mac(b'PDUCLI01',frame)).hex()}
    for stage, codes in [(1,[0]),(2,[0,3,4,5,6])]:
        for code in codes:
            prefix = b'PDUPACK1'+struct.pack('>II',stage,code)
            result[f'ack_{stage}_{code}'] = (prefix+channel.mac(b'PDUACK01',prefix)).hex()
    return json.dumps(result, indent=2)+'\n'

if __name__ == '__main__':
    DESTINATION.write_text(vectors())
