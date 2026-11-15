#!/usr/bin/env python3
"""Native approval/observed-radio admission without sockets or radio mutations."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
import approval

CHECKER=None
SERIAL='2125-1213-8600-8895'
FIRMWARE='4.3.16.42034'


class ApprovalTest(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name);self.path=self.root/'approval'
        self.good=approval.encode(1,FIRMWARE,SERIAL,bytes(range(32)))

    def check(self,record=None,serial=SERIAL,firmware=FIRMWARE,model='FLEX-8600',mode=0o600,expected=0):
        if self.path.exists():self.path.chmod(0o600)
        self.path.write_bytes(self.good if record is None else record);self.path.chmod(mode)
        reply=f'R970201|0|model="{model}",software_ver={firmware},chassis_serial="{serial}"'
        result=subprocess.run([str(CHECKER),str(self.path),reply],capture_output=True,timeout=5)
        self.assertEqual(result.returncode,expected,result.stderr.decode(errors='replace'))

    def test_exact_radio_and_6300_profile(self):
        self.check()
        self.check(approval.encode(2,'4.2.18.41174','6300-test',bytes(32)),
                   model='FLEX-6300',firmware='4.2.18.41174',serial='6300-test')

    def test_different_radio_same_model_and_firmware_refused(self):
        self.check(serial='2125-1213-8600-8896',expected=1)

    def test_other_firmware_or_model_refused(self):
        self.check(firmware='4.3.16.42035',expected=1)
        self.check(model='FLEX-6300',expected=1)

    def test_old_version_lengths_and_reserved_fields(self):
        for data in [b'PDAPRV01'+self.good[8:76],self.good[:-1],self.good+b'\0']:
            self.check(data,expected=1)
        for offset in [0,8,9,10,12,13,14,15]:
            data=bytearray(self.good);data[offset]^=1;self.check(data,expected=1)
        for profile in [0,3,255]:
            data=bytearray(self.good);data[11]=profile;self.check(data,expected=1)

    def test_text_bounds_padding_and_characters(self):
        for offset,size,values in [(48,28,[b'',b'4.3.16',b'4..16.42034',b'4.3.16.42034.',b'x'*28]),
                                   (76,64,[b'',b'x'*64,b'bad serial',b'../bad/path',b'bad\nserial'])]:
            for value in values:
                data=bytearray(self.good);data[offset:offset+size]=value.ljust(size,b'\0')
                self.check(data,expected=1)
            data=bytearray(self.good);data[offset+size-1]=1;self.check(data,expected=1)
        self.check(approval.encode(1,FIRMWARE,'x'*63,bytes(32)),serial='x'*63)

    def test_protected_record_mode_links_and_nonregular(self):
        for mode in [0o644,0o640,0o400,0o4600]:self.check(mode=mode,expected=1)
        self.path.chmod(0o600);self.path.write_bytes(self.good)
        os.link(self.path,self.root/'alias');self.check(expected=1)
        (self.root/'alias').unlink();self.path.unlink()
        self.path.symlink_to(self.root/'elsewhere')
        reply=f'R970201|0|model=FLEX-8600,software_ver={FIRMWARE},chassis_serial={SERIAL}'
        result=subprocess.run([str(CHECKER),str(self.path),reply],timeout=5);self.assertEqual(result.returncode,1)
        self.path.unlink();os.mkfifo(self.path,0o600)
        result=subprocess.run([str(CHECKER),str(self.path),reply],timeout=5);self.assertEqual(result.returncode,1)

    def test_encoder_rejects_ambiguous_targets(self):
        for profile,firmware,serial,digest in [(0,FIRMWARE,SERIAL,bytes(32)),(1,'4.3.16',SERIAL,bytes(32)),
                (1,FIRMWARE,'',bytes(32)),(1,FIRMWARE,'x'*64,bytes(32)),(1,FIRMWARE,'a\0b',bytes(32)),
                (1,FIRMWARE,SERIAL,bytes(31))]:
            with self.assertRaises(ValueError):approval.encode(profile,firmware,serial,digest)


if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--checker',type=Path,required=True)
    args,rest=parser.parse_known_args();CHECKER=args.checker.resolve()
    unittest.main(argv=['test_approval.py',*rest])
