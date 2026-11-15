#!/usr/bin/env python3
"""Outer release identity, operator-digest and privilege-file boundary tests."""
import argparse
import hashlib
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
import bundle
import release
import units

CHECKER = None


class ReleaseTest(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(prefix='aetherpd-release-');self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name)
        self.seed=hashlib.sha256(b'outer release fixture key, not a shipping trust anchor').digest()
        self.signer=Ed25519PrivateKey.from_private_bytes(self.seed)
        self.key=self.root/'key';self.key.write_bytes(self.signer.public_key().public_bytes_raw())
        self.raw=bytearray(4096);self.raw[:7]=b'\x7fELF\x02\x01\x01';struct.pack_into('<HH',self.raw,16,2,183)
        self.payload_files={n:bytes(self.raw) if n!='LICENSES' else b'fixture' for n in bundle.NAMES}
        payload,_=bundle.assemble(self.payload_files,64,bytes(32),self.seed)
        self.files={'pd-boot':bytes(self.raw),'pd-update':bytes(self.raw),'payload.bundle':payload,
                    'BOOT-LICENSES.txt':b'fixture licenses',**{n:s.encode() for n,s in units.render('arm64').items()}}
        self.good,self.plan=release.assemble(self.files,'flex8600','4.3.16.42034',bytes(32),self.seed)

    def check(self,data=None,expected=0,extract=False,arch=64,profile=1,firmware='4.3.16.42034',approved=None,swap=None):
        data=self.good if data is None else data
        input=self.root/'release';input.write_bytes(data)
        digest=self.root/'approved';digest.write_bytes(bundle.blake(data) if approved is None else approved)
        args=[str(CHECKER),str(input),str(self.key),str(arch),str(profile),firmware,str(digest)]
        if extract:
            output=self.root/'out';output.mkdir(exist_ok=True);args.append(str(output))
        if swap is not None:
            replacement=self.root/'swap';replacement.write_bytes(swap);args.append(str(replacement))
        result=subprocess.run(args,capture_output=True,timeout=10)
        self.assertEqual(result.returncode,expected,result.stderr.decode(errors='replace'))

    def resign(self,data):
        data=bytearray(data);data[release.MANIFEST:release.PREFIX]=self.signer.sign(data[:release.MANIFEST]);return bytes(data)

    def replace(self,index,raw):
        records=[];data=bytearray(self.good[:release.MANIFEST]);offset=release.PREFIX
        for i in range(len(release.NAMES)):
            size=struct.unpack_from('>I',data,release.HEADER+36*i)[0]
            records.append(raw if i==index else self.good[offset:offset+size]);offset+=size
        struct.pack_into('>I',data,release.HEADER+36*index,len(raw))
        data[release.HEADER+36*index+4:release.HEADER+36*index+36]=bundle.blake(raw)
        return self.resign(data+bytes(64)+b''.join(records))

    def test_exact_fixed_release_and_reproducibility(self):
        other,plan=release.assemble(self.files,'flex8600','4.3.16.42034',bytes(32),self.seed)
        self.assertEqual(other,self.good);self.assertEqual(plan,self.plan)
        self.check(extract=True)
        for name in release.NAMES:
            self.assertEqual((self.root/'out'/name).read_bytes(),self.files[name])
        self.check(expected=1,extract=True)

    def test_signed_paired_boot_contract_and_legacy_compatibility(self):
        self.assertEqual(self.good[:8], b'AEPDR002')
        self.assertTrue(self.plan['paired_boot'])
        self.check()
        legacy, plan=release.assemble(self.files,'flex8600','4.3.16.42034',bytes(32),self.seed,paired_boot=False)
        self.assertEqual(legacy[:8],b'AEPDR001');self.assertFalse(plan['paired_boot']);self.check(legacy)
        altered=bytearray(self.good);altered[7]=ord('1');self.check(altered,expected=1)
        altered[7]=ord('6');self.check(self.resign(altered),expected=1)

    def test_origin_recovery_signed_contract(self):
        files={**self.files,**{n:s.encode() for n,s in units.render('arm64',True).items()}}
        data,plan=release.assemble(files,'flex8600','4.3.16.42034',bytes(32),self.seed,
                                   managed_startup=True,origin_recovery=True)
        self.assertEqual(data[:8],b'AEPDR004');self.assertTrue(plan['origin_recovery'])
        self.check(data,extract=True)
        changed=bytearray(data);changed[7]=ord('3');self.check(changed,expected=1)
        with self.assertRaises(ValueError):
            release.assemble(self.files,'flex8600','4.3.16.42034',bytes(32),self.seed,origin_recovery=True)

    def test_generation_selection_signed_contract(self):
        files={**self.files,**{n:s.encode() for n,s in units.render('arm64',True).items()}}
        data,plan=release.assemble(files,'flex8600','4.3.16.42034',bytes(32),self.seed,
                                   managed_startup=True,origin_recovery=True,generation_selection=True)
        self.assertEqual(data[:8],b'AEPDR005');self.assertTrue(plan['generation_selection'])
        self.check(data,extract=True)
        changed=bytearray(data);changed[7]=ord('4');self.check(changed,expected=1)
        with self.assertRaises(ValueError):
            release.assemble(files,'flex8600','4.3.16.42034',bytes(32),self.seed,
                             managed_startup=True,generation_selection=True)

    def test_managed_startup_signed_contract(self):
        files={**self.files, **{n:s.encode() for n,s in units.render('arm64', True).items()}}
        data,plan=release.assemble(files,'flex8600','4.3.16.42034',bytes(32),self.seed,managed_startup=True)
        self.assertEqual(data[:8],b'AEPDR003');self.assertTrue(plan['managed_startup'])
        self.assertFalse(plan['origin_recovery'])
        self.check(data,extract=True)
        self.assertIn(b'pd-boot --session', (self.root/'out/aetherpd-start@.service').read_bytes())
        self.assertIn(b'pd-boot --primary', (self.root/'out/aetherpd-shim.service').read_bytes())
        # Even a correctly signed envelope cannot mislabel the fixed layout.
        for magic,original in [(b'AEPDR003',self.good),(b'AEPDR002',data),(b'AEPDR001',data)]:
            self.check(self.resign(magic+original[8:]),expected=1)
        for managed,inputs,paired in [(True,self.files,True),(False,files,True),(True,files,False)]:
            with self.assertRaises(ValueError):
                release.assemble(inputs,'flex8600','4.3.16.42034',bytes(32),self.seed,
                                 paired_boot=paired,managed_startup=managed)
        old=self.good;self.good=data
        for index in (6,7):
            self.check(self.replace(index,self.files[release.NAMES[index]]),expected=1)
            self.check(self.replace(index,files[release.NAMES[index]]+b'ExecStartPost=/bin/sh\n'),expected=1)
        self.good=old

    def test_approved_digest_binds_exact_release(self):
        self.check(approved=bytes(32),expected=1)
        changed=self.replace(3,b'another valid signed license')
        self.check(changed,approved=bundle.blake(self.good),expected=1)
        self.check(changed)

    def test_signature_key_and_all_records(self):
        for offset in [0,release.MANIFEST,release.PREFIX,len(self.good)-1]:
            data=bytearray(self.good);data[offset]^=1;self.check(data,expected=1)
        self.key.write_bytes(Ed25519PrivateKey.generate().public_key().public_bytes_raw())
        self.check(expected=1)

    def test_profile_firmware_architecture(self):
        for arch,profile,firmware in [(7,2,'4.3.16.42034'),(64,2,'4.3.16.42034'),(64,1,'4.3.16.42035'),(64,1,''),(64,1,'4.3.16')]:
            self.check(arch=arch,profile=profile,firmware=firmware,expected=1)
        data=bytearray(self.good);data[79]=1;self.check(self.resign(data),expected=1)

    def test_signed_units_cannot_add_commands(self):
        for index in range(4,8):
            self.check(self.replace(index,self.files[release.NAMES[index]]+b'ExecStartPost=/bin/sh\n'),expected=1)

    def test_signed_wrong_helper_architecture(self):
        data=bytearray(self.raw);data[4]=1
        for index in [0,1]:self.check(self.replace(index,data),expected=1)

    def test_lengths_and_trailing_data(self):
        self.check(self.good[:-1],expected=1);self.check(self.good+b'extra',expected=1)
        for value in [0,0xffffffff]:
            data=bytearray(self.good);struct.pack_into('>I',data,release.HEADER,value)
            self.check(self.resign(data),expected=1)

    def test_authenticated_bad_inner_payload_cannot_extract(self):
        raw=bytearray(self.files['payload.bundle']);raw[-1]^=1
        self.check(self.replace(2,raw),expected=1,extract=True)
        self.assertFalse((self.root/'out/aetherpd-shim.service').exists())

    def test_output_symlink_refused(self):
        out=self.root/'out';out.mkdir();victim=self.root/'victim';victim.write_bytes(b'preserve')
        (out/'pd-boot').symlink_to(victim)
        self.check(expected=1,extract=True);self.assertEqual(victim.read_bytes(),b'preserve')

    def test_changed_input_after_approval_refused(self):
        changed=self.replace(3,b'a different signed release')
        self.check(expected=1,extract=True,swap=changed)
        self.assertFalse((self.root/'out/aetherpd-shim.service').exists())

    def test_builder_rejects_foreign_files_units_and_profiles(self):
        for files,profile,firmware in [({**self.files,'../escape':b'x'},'flex8600','4.3.16.42034'),
                ({**self.files,'aetherpd-shim.service':b'ExecStart=/bin/sh'},'flex8600','4.3.16.42034'),
                (self.files,'unknown','4.3.16.42034'),(self.files,'flex8600','4.3.16.42034\n')]:
            with self.assertRaises(ValueError):release.assemble(files,profile,firmware,bytes(32),self.seed)

    def test_armv7_profile(self):
        raw=bytearray(self.raw);raw[4]=1;struct.pack_into('<H',raw,18,40)
        payload,_=bundle.assemble({n:bytes(raw) if n!='LICENSES' else b'fixture' for n in bundle.NAMES},7,bytes(32),self.seed)
        files={**self.files,'pd-boot':bytes(raw),'pd-update':bytes(raw),'payload.bundle':payload,
               **{n:s.encode() for n,s in units.render('armv7').items()}}
        for managed in (False,True):
            files.update({n:s.encode() for n,s in units.render('armv7',managed).items()})
            data,_=release.assemble(files,'flex6300','4.2.18.41174',bytes(32),self.seed,managed_startup=managed)
            self.check(data,arch=7,profile=2,firmware='4.2.18.41174')


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--checker',type=Path,required=True)
    args,rest=p.parse_known_args();CHECKER=args.checker.resolve()
    unittest.main(argv=['test_release.py',*rest])
