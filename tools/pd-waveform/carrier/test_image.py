#!/usr/bin/env python3
"""The public carrier artifact must not persist the private request credential."""
import gzip
import io
import json
from pathlib import Path
import struct
import sys
import tarfile
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'request'))
import client
import image

class ImageTest(unittest.TestCase):
    def setUp(self):
        self.private=b'PDXFER01'+bytes(range(1,33))+b'PDDELIV1'+struct.pack('>II',64,2)+b'e'*32+b'p'*32
        self.public=client.public_record(self.private)
        binary=bytearray(64);binary[:6]=b'\x7fELF\x02\x01';struct.pack_into('<H',binary,18,183)
        self.executables={name:bytes(binary) for name in ['accept','carrier','send','deliver']}
    def layers(self,data):
        with tarfile.open(fileobj=io.BytesIO(gzip.decompress(data))) as archive:
            index=json.load(archive.extractfile('index.json'))
            manifest=json.load(archive.extractfile('blobs/sha256/'+index['manifests'][0]['digest'][7:]))
            layer=archive.extractfile('blobs/sha256/'+manifest['layers'][0]['digest'][7:]).read()
        return gzip.decompress(layer)
    def test_reproducible_public_artifact_contains_no_private_configuration(self):
        first,info=image.build(self.executables,self.public,b'licenses')
        second,again=image.build(self.executables,self.public,b'licenses')
        self.assertEqual(first,second);self.assertEqual(info,again)
        layer=self.layers(first)
        self.assertNotIn(self.private,layer);self.assertNotIn(self.private[8:40],layer)
        with tarfile.open(fileobj=io.BytesIO(layer)) as archive:
            names=set(archive.getnames())
            self.assertEqual(names,{'pd','pd/accept','pd/carrier','pd/send','pd/deliver','pd/request','LICENSES'})
            self.assertEqual(archive.extractfile('pd/request').read(),self.public)
            self.assertEqual(archive.getmember('pd/request').mode,0o444)
    def test_private_and_unsupported_requests_refused(self):
        for bad in [self.private,self.public[:-1],self.public+bytes(1),b'x'+self.public[1:],self.public[:88]+bytes(32),self.public[:16]+struct.pack('>II',7,2)+self.public[24:],self.public[:20]+struct.pack('>I',1)+self.public[24:]]:
            with self.subTest(kind=bad[:8]):
                with self.assertRaises(ValueError):image.build(self.executables,bad,b'licenses')
    def test_public_commitment_binds_private_key_and_receipt(self):
        expected=b'PDPUB001'+self.private[40:]+client.digest(b'PDKEY001'+self.private)
        self.assertEqual(self.public,expected)
        for position in [8,56,88]:
            changed=bytearray(self.private);changed[position]^=1
            self.assertNotEqual(client.public_record(bytes(changed)),self.public)
        with self.assertRaises(ValueError):client.public_record(self.private[:52]+struct.pack('>I',1)+self.private[56:])

if __name__=='__main__':unittest.main()
