#!/usr/bin/env python3
"""Refuse first-install uploads whose archives differ from operator consent."""
import hashlib
import gzip
import importlib.util
import io
import json
from pathlib import Path
import struct
import sys
import tarfile
import unittest
import first_install_package as package

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'boot'))
import approval
import bootstrap_artifact
import bundle
import first_install
import release
import units
spec = importlib.util.spec_from_file_location('first_image', Path(__file__).resolve().parents[1]/'carrier/image.py')
image = importlib.util.module_from_spec(spec)
spec.loader.exec_module(image)


class FirstPackageTest(unittest.TestCase):
    def setUp(self):
        seed = hashlib.sha256(b'public first-package test fixture; not a release key').digest()
        binary = bytearray(128)
        binary[:7] = b'\x7fELF\x02\x01\x01'
        struct.pack_into('<HH', binary, 16, 2, 183)
        self.binary = bytes(binary)
        payload, _ = bundle.assemble({n:self.binary if n != 'LICENSES' else b'fixture' for n in bundle.NAMES}, 64, bytes(32), seed)
        self.release, _ = release.assemble({'pd-boot':self.binary, 'pd-update':self.binary,
            'payload.bundle':payload, 'BOOT-LICENSES.txt':b'fixture',
            **{n:v.encode() for n,v in units.render('arm64').items()}}, 'flex8600', '4.3.16.42034', bytes(32), seed)
        self.worker = bootstrap_artifact.assemble(self.binary, 64, 1, bytes(32), bundle.blake(self.release), seed)
        self.plan = first_install.encode(approval.encode(1,'4.3.16.42034','fixture-8600',bundle.blake(self.release)),
                                         b't'*32,'fresh-radio',b'n'*32)

    def artifacts(self, plan):
        name, native, public = package.build(self.binary, self.worker, plan, self.release, b'fixture')
        carrier, info = image.build({n:self.binary for n in ['accept','carrier','send','deliver']}, public, b'fixture')
        return name, info['name'], native, carrier

    def test_exact_archives_match_approved_plan(self):
        native_name, carrier_name, native, carrier = self.artifacts(self.plan)
        self.assertTrue(native_name.isalnum(), "legacy firmware strips hyphenated native prefixes")
        self.assertEqual(package.verify_uploads(native, carrier, self.plan), (native_name, carrier_name))

    def test_archive_with_different_token_identity_refused(self):
        _, _, native, carrier = self.artifacts(self.plan)
        changed = bytearray(self.plan)
        changed[148] ^= 1
        with self.assertRaises(ValueError):
            package.verify_uploads(native, carrier, bytes(changed))

    def test_carrier_for_different_plan_refused(self):
        _, _, native, _ = self.artifacts(self.plan)
        changed = bytearray(self.plan)
        changed[148] ^= 1
        _, _, _, other = self.artifacts(bytes(changed))
        with self.assertRaises(ValueError):
            package.verify_uploads(native, other, self.plan)

    def test_desktop_producer_accepted(self):
        name, native, public = package.build(self.binary, self.worker, self.plan, self.release, b'fixture')
        carrier, info = image.build({n:self.binary for n in ['accept','carrier','send','deliver']},
                                    public, b'fixture', producer='AetherSDR AetherPDCarrierImage')
        self.assertEqual(package.verify_uploads(native, carrier, self.plan), (name, info['name']))

    def test_unknown_producer_refused(self):
        _, _, public = package.build(self.binary, self.worker, self.plan, self.release, b'fixture')
        for producer in [None, 'unrecognized', '', ['AetherSDR AetherPDCarrierImage']]:
            with self.subTest(producer=producer):
                with self.assertRaises(ValueError):
                    image.build({n:self.binary for n in ['accept','carrier','send','deliver']},
                                public, b'fixture', producer=producer)

    def test_outer_metadata_changes_refused(self):
        _, _, native, carrier = self.artifacts(self.plan)
        with tarfile.open(fileobj=io.BytesIO(gzip.decompress(carrier))) as archive:
            entries = [(m.name, archive.extractfile(m).read() if m.isfile() else None, m.mode)
                       for m in archive.getmembers()]
        def changed_index(rows):
            result = []
            for name, data, mode in rows:
                if name == 'index.json':
                    value = json.loads(data)
                    value['manifests'][0]['platform']['architecture'] = 'arm'
                    data = json.dumps(value).encode()
                result.append((name, data, mode))
            return result
        for rows in [changed_index(entries), entries + [('extra', b'not allowed', 0o644)],
                     [(n, d, 0o777 if n == 'index.json' else m) for n, d, m in entries]]:
            with self.assertRaises(ValueError):
                package.verify_uploads(native, image.helpers.gz(image.helpers.tar_bytes(rows)), self.plan)


if __name__ == '__main__':
    unittest.main()
