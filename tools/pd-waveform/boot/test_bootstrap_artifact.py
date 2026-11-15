#!/usr/bin/env python3
"""Privileged executable provenance and exact release/role binding tests."""
import argparse
import hashlib
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
import bootstrap_artifact as artifact
import bundle
import release
import units

CHECKER = None
RELEASE_CHECKER = None


class ArtifactTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='pd-bootstrap-test-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.seed = hashlib.sha256(b'bootstrap artifact fixture only').digest()
        self.signer = Ed25519PrivateKey.from_private_bytes(self.seed)
        self.key = self.signer.public_key().public_bytes_raw()
        self.source = hashlib.sha256(b'fixture source').digest()
        self.release = bundle.blake(b'fixture release')
        self.worker = bytearray(4096)
        self.worker[:7] = b'\x7fELF\x02\x01\x01'
        struct.pack_into('<HH', self.worker, 16, 2, 183)
        self.good = artifact.assemble(bytes(self.worker), 64, 2, self.source, self.release, self.seed)

    def check(self, data=None, accepted=False, arch=64, role=2, key=None, source=None, release=None):
        for name, raw in [('artifact', self.good if data is None else data),
                          ('key', self.key if key is None else key),
                          ('source', self.source if source is None else source),
                          ('release', self.release if release is None else release)]:
            (self.root/name).write_bytes(raw)
        result = subprocess.run([str(CHECKER), str(self.root/'artifact'), str(self.root/'key'),
            str(arch), str(role), str(self.root/'source'), str(self.root/'release')],
            capture_output=True, timeout=10)
        self.assertEqual(result.stderr, b'', result.stderr.decode())
        self.assertEqual(result.returncode, 0 if accepted else 1, result.stderr.decode())
        return result.stdout.decode().strip()

    def resign(self, data):
        return bytes(data[:artifact.HEADER])+self.signer.sign(bytes(data[:artifact.HEADER]))+bytes(data[artifact.PREFIX:])

    def test_exact_snapshot_and_reproducibility(self):
        self.assertEqual(self.good, artifact.assemble(bytes(self.worker), 64, 2, self.source, self.release, self.seed))
        self.assertEqual(self.check(accepted=True), f'{len(self.worker)} {bundle.blake(self.worker).hex()} {bundle.blake(self.good).hex()}')

    def test_wrong_publisher(self):
        self.check(key=Ed25519PrivateKey.from_private_bytes(bytes(32)).public_key().public_bytes_raw())

    def test_release_source_role_and_architecture_substitution(self):
        self.check(release=bytes(32));self.check(source=bytes(32))
        self.check(role=1);self.check(role=3);self.check(arch=7);self.check(arch=0)

    def test_unsigned_manifest_changes(self):
        for offset in [0, 8, 12, 16, 20, 24, 56, 88, artifact.HEADER, artifact.PREFIX-1]:
            with self.subTest(offset=offset):
                data=bytearray(self.good);data[offset]^=1;self.check(data)

    def test_worker_tampering(self):
        for offset in [artifact.PREFIX, artifact.PREFIX+200, len(self.good)-1]:
            data=bytearray(self.good);data[offset]^=1;self.check(data)

    def test_truncation_trailing_bytes_and_size_bounds(self):
        for size in [0, 7, 24, artifact.HEADER, artifact.PREFIX, len(self.good)-1]:
            self.check(self.good[:size])
        self.check(self.good+b'x')
        self.check(self.good+bytes(artifact.MAX_WORKER))
        for size in [0, 51, artifact.MAX_WORKER+1, 0xffffffff]:
            data=bytearray(self.good);struct.pack_into('>I',data,16,size);self.check(self.resign(data))

    def test_signed_unknown_format_and_reserved_bits(self):
        for offset in [7,20,21,22,23]:
            data=bytearray(self.good);data[offset]^=1;self.check(self.resign(data))

    def test_signed_invalid_elf(self):
        for offset in [0,4,5,6,16,17,18,19]:
            data=bytearray(self.good);data[artifact.PREFIX+offset]^=1
            data[88:120]=bundle.blake(data[artifact.PREFIX:]);self.check(self.resign(data))

    def test_current_installer_only_supports_arm64(self):
        data=artifact.assemble(bytes(self.worker),64,1,self.source,self.release,self.seed)
        self.check(data,accepted=True,role=1)
        worker=bytearray(self.worker);worker[4]=1;struct.pack_into('<H',worker,18,40)
        with self.assertRaises(ValueError):artifact.assemble(worker,7,1,self.source,self.release,self.seed)
        data=artifact.assemble(worker,7,2,self.source,self.release,self.seed)
        self.check(data,accepted=True,arch=7)
        changed=bytearray(data);struct.pack_into('>I',changed,12,1)
        self.check(self.resign(changed),arch=7,role=1)

    def test_publisher_rejects_oversize_and_non_executable(self):
        for worker in [b'', bytes(artifact.MAX_WORKER+1), b'#!/bin/sh\n'+bytes(64)]:
            with self.assertRaises(ValueError):artifact.assemble(worker,64,2,self.source,self.release,self.seed)

    def test_publisher_verifies_release_before_signing(self):
        files={name:bytes(self.worker) if name!='LICENSES' else b'fixture license' for name in bundle.NAMES}
        payload,_=bundle.assemble(files,64,self.source,self.seed)
        outer,_=release.assemble({'pd-boot':bytes(self.worker),'pd-update':bytes(self.worker),
            'payload.bundle':payload,'BOOT-LICENSES.txt':b'fixture license',
            **{name:text.encode() for name,text in units.render('arm64').items()}},
            'flex8600','4.3.16.42034',self.source,self.seed)
        worker=self.root/'worker';worker.write_bytes(self.worker)
        package=self.root/'package';package.write_bytes(outer)
        seed=self.root/'seed';seed.write_bytes(self.seed);seed.chmod(0o600)
        output=self.root/'signed'
        report=artifact.publish(worker,package,seed,'upgrade',output,RELEASE_CHECKER)
        self.assertFalse(report['executes']);self.assertFalse(report['grants_consent'])
        expected=artifact.assemble(self.worker,64,2,self.source,bundle.blake(outer),self.seed)
        self.assertEqual(output.read_bytes(),expected)
        self.check(expected,accepted=True,release=bundle.blake(outer))
        for bad in [outer[:-1], outer[:release.PREFIX]+bytes(len(outer)-release.PREFIX)]:
            package.write_bytes(bad)
            with self.assertRaises(subprocess.CalledProcessError):
                artifact.publish(worker,package,seed,'upgrade',output,RELEASE_CHECKER)
            self.assertEqual(output.read_bytes(),expected)
        package.write_bytes(outer);seed.chmod(0o644)
        with self.assertRaises(ValueError):artifact.publish(worker,package,seed,'upgrade',output,RELEASE_CHECKER)
        seed.chmod(0o600);seed.write_bytes(bytes(32))
        with self.assertRaises(subprocess.CalledProcessError):
            artifact.publish(worker,package,seed,'upgrade',output,RELEASE_CHECKER)

    def test_publisher_rejects_links_and_special_files(self):
        path=self.root/'file';path.write_bytes(self.seed)
        link=self.root/'link';link.symlink_to(path)
        with self.assertRaises(OSError):artifact.bounded_file(link,32,32)
        with self.assertRaises(ValueError):artifact.bounded_file(self.root,1,4096)


if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--checker',type=Path,required=True)
    parser.add_argument('--release-checker',type=Path,required=True)
    args,remaining=parser.parse_known_args();CHECKER=args.checker.resolve();RELEASE_CHECKER=args.release_checker.resolve()
    unittest.main(argv=[__file__,*remaining])
