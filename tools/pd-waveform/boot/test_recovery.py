#!/usr/bin/env python3
"""Recovery recipe and separately admitted fixed rollback container; no sockets."""
import argparse
import hashlib
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
import approval
import migration
from test_predecessor import inventory, encoded

CHECKER=None

class RecoveryTest(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(prefix='pd-recovery-');self.addCleanup(self.temp.cleanup)
        self.store=Path(self.temp.name);self.store.chmod(0o700)
        target=approval.encode(1,'4.3.16.42034','fixture-8600',bytes(range(32)))
        self.approval=migration.encode(target,'a'*64,'b'*64,bytes(range(32)))
        self.raw=b'PDREC001'+self.approval+b'192.168.50.100'.ljust(16,b'\0')
        self.recipe=self.store/'recovery-recipe';self.recipe.write_bytes(self.raw);self.recipe.chmod(0o600)
        self.hostname='aetherpd-'+hashlib.blake2b(self.raw,digest_size=32).hexdigest()[:32]

    def run_check(self,mode,data,accepted=False,extra=()):
        p=subprocess.run([str(CHECKER),mode,*map(str,extra)],input=data,capture_output=True,timeout=5)
        self.assertEqual(p.returncode,0 if accepted else 1,p.stderr.decode(errors='replace'))
        return p.stdout.decode()

    def restored(self):
        fields=inventory();fields[0]='PDREST01';fields[1]='c'*64
        fields[16]='["/var/lib/smartsdr/aetherpd/legacy/state:/var/lib/flex-tailnet-shim:rw,rprivate,nosuid,rbind"]'
        fields[22]='RESTORED';fields[32]=encoded('HOSTNAME='+self.hostname)
        fields[33]=encoded('SSDR_RADIO_ADDRESS=192.168.50.100')
        fields += [encoded(self.hostname),'1',encoded('bind'),encoded('/var/lib/smartsdr/aetherpd/legacy/state'),
                   encoded('/var/lib/flex-tailnet-shim'),'','','["nosuid","rbind"]','true',encoded('rprivate')]
        return fields

    def check_restored(self,fields,accepted=False):
        return self.run_check('--restored',('\n'.join(fields)+'\n').encode(),accepted,[self.recipe])

    def test_recipe_preserves_address_and_derives_hostname(self):
        result=self.run_check('--recipe',self.raw,True)
        self.assertEqual(result,'192.168.50.100\n'+self.hostname+'\n')
        changed=self.raw[:316]+b'127.0.0.1'.ljust(16,b'\0')
        result=self.run_check('--recipe',changed,True)
        self.assertNotIn(self.hostname,result)

    def test_recipe_framing_and_approval(self):
        for raw in [b'',self.raw[:-1],self.raw+b'x',b'PDREC002'+self.raw[8:],
                    self.raw[:8]+b'PDMIG002'+self.raw[16:],self.raw[:316]+b'\0'*16,
                    self.raw[:156]+b'0'*64+self.raw[220:]]:
            with self.subTest(raw=raw[:20]):self.run_check('--recipe',raw)

    def test_recipe_ipv4_is_canonical_and_padded(self):
        for address in [b'127.000.0.1',b'127.1',b'::1',b'999.1.1.1',b'1.2.3.4\nx',b'--all',b'1.2.3.4\0x',b'x'*16]:
            with self.subTest(address=address):self.run_check('--recipe',self.raw[:316]+address.ljust(16,b'\0'))
        for address in [b'127.0.0.1',b'172.30.1.1',b'192.168.50.100']:
            self.run_check('--recipe',self.raw[:316]+address.ljust(16,b'\0'),True)

    def test_read_requires_matching_approval(self):
        self.run_check('--read-recipe',self.approval,True,[self.store])
        altered=self.approval[:148]+b'c'*64+self.approval[212:]
        self.run_check('--read-recipe',altered,False,[self.store])

    def test_read_checks_metadata(self):
        self.recipe.chmod(0o644);self.run_check('--read-recipe',self.approval,False,[self.store])
        self.recipe.chmod(0o600);os.link(self.recipe,self.store/'alias')
        self.run_check('--read-recipe',self.approval,False,[self.store]);self.recipe.unlink()
        self.recipe.symlink_to(self.store/'alias');self.run_check('--read-recipe',self.approval,False,[self.store])

    def test_read_checks_directory(self):
        self.store.chmod(0o777);self.run_check('--read-recipe',self.approval,False,[self.store])
        self.store.chmod(0o700)

    def test_restored_running_and_stopped(self):
        fields=self.restored();self.check_restored(fields,True)
        fields[25]='false';fields[26]='0';self.check_restored(fields,True)
        fields[26]='42';self.check_restored(fields)

    def test_original_and_restored_admission_are_distinct(self):
        self.check_restored(inventory())
        self.run_check('--status',('\n'.join(self.restored())+'\n').encode())

    def test_restored_binds_only_one_fixed_private_directory(self):
        for index,value in [(16,'[]'),(16,'["/:/host:rw"]'),(22,'[]'),(36,'0'),(36,'2'),
                            (37,encoded('volume')),(38,encoded('/tmp/state')),
                            (39,encoded('/host')),(40,encoded('local')),(41,encoded('z')),
                            (42,'["rbind"]'),(42,'["nosuid","rbind","shared"]'),
                            (43,'false'),(44,encoded('shared'))]:
            with self.subTest(index=index,value=value):
                fields=self.restored();fields[index]=value;self.check_restored(fields)

    def test_restored_requires_recipe_identity(self):
        for index,value in [(1,'a'*64),(2,'d'*64),(3,encoded('foreign-shim')),
                            (32,encoded('HOSTNAME=foreign')),(33,encoded('SSDR_RADIO_ADDRESS=127.0.0.1')),
                            (35,encoded('foreign'))]:
            with self.subTest(index=index):
                fields=self.restored();fields[index]=value;self.check_restored(fields)

    def test_restored_keeps_original_privilege_boundaries(self):
        for index,value in [(4,encoded('1000')),(5,encoded('/bin/sh')),(11,encoded('bridge')),
                            (12,encoded('host')),(14,'true'),(17,'["CAP_SYS_ADMIN"]'),
                            (18,'[]'),(19,'["/dev/mem"]'),(21,'["label=disable"]'),(23,encoded('no'))]:
            with self.subTest(index=index):
                fields=self.restored();fields[index]=value;self.check_restored(fields)

    def test_restored_framing(self):
        good=('\n'.join(self.restored())+'\n').encode()
        for data in [good[:-1],good+b'\n',b'\0'+good,good+b'foreign\n',b'x'*8192+b'\n']:
            self.run_check('--restored',data,False,[self.recipe])

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--checker',type=Path,required=True)
    args,rest=parser.parse_known_args();CHECKER=args.checker.resolve()
    unittest.main(argv=['test_recovery.py',*rest])
