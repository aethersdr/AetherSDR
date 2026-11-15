#!/usr/bin/env python3
"""Authenticated bundle boundary tests against the actual native verifier."""
import argparse
import gzip
import hashlib
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
import bundle

CHECKER = None


class BundleTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='aetherpd-bundle-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.seed = hashlib.sha256(b'test key only; never a shipped trust anchor').digest()
        key = Ed25519PrivateKey.from_private_bytes(self.seed)
        self.key = self.root / 'public-key'
        self.key.write_bytes(key.public_key().public_bytes_raw())
        self.files = {}
        for i, name in enumerate(bundle.NAMES):
            data = bytearray(4096 + i)
            data[:7] = b'\x7fELF\x02\x01\x01'
            struct.pack_into('<HH', data, 16, 2, 183)
            self.files[name] = bytes(data) if name != 'LICENSES' else b'Test license\n'
        self.good, self.report = bundle.assemble(self.files, 64, bytes(32), self.seed)

    def check(self, data, accepted, extract=False, arch=64):
        path = self.root / 'input.bundle'
        path.write_bytes(data)
        args = [str(CHECKER), str(path), str(self.key), str(arch)]
        if extract:
            output = self.root / 'output'
            output.mkdir(exist_ok=True)
            args.append(str(output))
        result = subprocess.run(args, capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0 if accepted else 1, result.stderr.decode(errors='replace'))

    def resign(self, data):
        manifest_size = bundle.PREFIX - 64
        data = bytearray(data)
        data[manifest_size:bundle.PREFIX] = Ed25519PrivateKey.from_private_bytes(self.seed).sign(data[:manifest_size])
        return bytes(data)

    def test_roundtrip_reproducibility(self):
        again, _ = bundle.assemble(self.files, 64, bytes(32), self.seed)
        self.assertEqual(self.good, again)
        self.check(self.good, True, extract=True)
        for name in bundle.NAMES:
            self.assertEqual((self.root / 'output' / name).read_bytes(), self.files[name])
        self.check(self.good, False, extract=True)  # O_EXCL: never replace an existing payload

    def test_authentication_and_bounds(self):
        self.check(self.good, False, arch=7)
        for offset in [0, 8, 12, 48, bundle.PREFIX - 1, bundle.PREFIX, len(self.good) - 1]:
            bad = bytearray(self.good); bad[offset] ^= 1
            self.check(bad, False)
        self.check(self.good[:-1], False)
        self.check(self.good + b'\0', False)
        other = Ed25519PrivateKey.generate().public_key().public_bytes_raw()
        self.key.write_bytes(other)
        self.check(self.good, False)

    def test_signed_invalid_sizes_and_header(self):
        for offset, value in [(8, 7), (12, 12), (48, 0xffffffff), (52, 0xffffffff), (52, 0)]:
            bad = bytearray(self.good);struct.pack_into('>I', bad, offset, value)
            self.check(self.resign(bad), False)
        bad = bytearray(self.good);bad[0] = ord('X')
        self.check(self.resign(bad), False)

    def test_decompression_and_expanded_hash(self):
        bad = bytearray(self.good);bad[48 + 40] ^= 1
        self.check(self.resign(bad), False, extract=True)

    def test_output_symlink_refused(self):
        out = self.root / 'output';out.mkdir()
        victim = self.root / 'victim';victim.write_bytes(b'keep')
        (out / bundle.NAMES[0]).symlink_to(victim)
        self.check(self.good, False, extract=True)
        self.assertEqual(victim.read_bytes(), b'keep')

    def test_signed_short_expansion_refused(self):
        bad = bytearray(self.good);struct.pack_into('>I', bad, 52, len(self.files[bundle.NAMES[0]]) - 1)
        self.check(self.resign(bad), False, extract=True)

    def replace_first_gzip(self, packed):
        old_size = struct.unpack_from('>I', self.good, 48)[0]
        data = bytearray(self.good[:bundle.PREFIX] + packed + self.good[bundle.PREFIX + old_size:])
        struct.pack_into('>I', data, 48, len(packed))
        data[56:88] = bundle.blake(packed)
        return self.resign(data)

    def test_signed_gzip_corruption(self):
        packed = bytearray(gzip.compress(self.files[bundle.NAMES[0]], mtime=0))
        packed[-8] ^= 1  # authenticated but invalid gzip CRC
        self.check(self.replace_first_gzip(packed), False, extract=True)

    def test_signed_concatenated_gzip_refused(self):
        packed = gzip.compress(self.files[bundle.NAMES[0]], mtime=0) + gzip.compress(b'extra', mtime=0)
        self.check(self.replace_first_gzip(packed), False, extract=True)

    def test_armv7_header(self):
        for name in bundle.NAMES[:-1]:
            data = bytearray(self.files[name]);data[4] = 1
            struct.pack_into('<H', data, 18, 40)
            self.files[name] = bytes(data)
        package, _ = bundle.assemble(self.files, 7, bytes(32), self.seed)
        self.check(package, True, extract=True, arch=7)

    def test_builder_fixed_names_and_machine(self):
        with self.assertRaises(ValueError):
            bundle.assemble({**self.files, '../escape': b'x'}, 64, bytes(32), self.seed)
        self.files[bundle.NAMES[0]] = b'not an executable'
        with self.assertRaises(ValueError):
            bundle.assemble(self.files, 64, bytes(32), self.seed)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--checker', type=Path, required=True)
    args, rest = parser.parse_known_args()
    CHECKER = args.checker.resolve()
    unittest.main(argv=['test_bundle.py', *rest])
