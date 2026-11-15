#!/usr/bin/env python3
"""Socket-free boundary checks for the fixed legacy container inventory."""
import argparse
from pathlib import Path
import subprocess
import unittest

CHECKER=None


def encoded(value):
    return value.encode().hex()


def inventory():
    fields=['PDLEG001','a'*64,'b'*64,encoded('flex-tailnet-shim'),'',
            encoded('/flex-tailnet-shim'),'null',encoded('/'),encoded('flex-tailnet-shim'),
            encoded('0.5.2'),'2',encoded('host'),encoded('private'),encoded('shareable'),
            'false','false','[]','[]','["CAP_AUDIT_WRITE","CAP_MKNOD","CAP_NET_RAW"]',
            '[]','{}','[]','[]',encoded('always'),'0','true','1234','5','ENV']
    fields += [encoded(value) for value in [
        'PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin',
        'TERM=xterm','container=podman','HOSTNAME=fixture-8600','SSDR_RADIO_ADDRESS=127.0.0.1']]
    return fields+['END']


class PredecessorTest(unittest.TestCase):
    def invoke(self,fields=None,raw=None,accepted=False,status=False):
        data=raw if raw is not None else ('\n'.join(fields if fields is not None else inventory())+'\n').encode()
        p=subprocess.run([str(CHECKER)]+(['--status'] if status else []),input=data,capture_output=True,timeout=5)
        self.assertEqual(p.returncode,0 if accepted else 1,p.stderr.decode(errors='replace'))

    def changed(self,index,value):
        fields=inventory();fields[index]=value;self.invoke(fields)

    def test_expected_configuration(self):
        self.invoke(accepted=True)
        fields=inventory();fields[29:34]=reversed(fields[29:34]);self.invoke(fields,accepted=True)

    def test_names_ids_and_executable(self):
        for index,value in [(1,'a'*63),(1,'a'*65),(1,'A'*64),(1,'--all'),(2,'b'*63),
                            (3,encoded('foreign-shim')),(4,encoded('1000')),
                            (5,encoded('/bin/sh')),(6,'["-state","/other"]'),(7,encoded('/tmp'))]:
            with self.subTest(index=index,value=value):self.changed(index,value)

    def test_labels_and_version(self):
        for index,value in [(8,encoded('other')),(9,encoded('0.5.3')),(10,'3')]:
            with self.subTest(index=index):self.changed(index,value)

    def test_namespaces_and_privileges(self):
        for index,value in [(11,encoded('bridge')),(12,encoded('host')),(13,encoded('host')),
                            (14,'true'),(15,'true'),(16,'["/:/host"]'),(17,'["CAP_SYS_ADMIN"]'),
                            (18,'[]'),(19,'[{"PathOnHost":"/dev/mem"}]'),(20,'{"80/tcp":[]}'),
                            (21,'["label=disable"]'),(22,'[{"Source":"/host"}]')]:
            with self.subTest(index=index):self.changed(index,value)

    def test_restart_and_running_state(self):
        for index,value in [(23,encoded('no')),(24,'1'),(25,'false')]:
            with self.subTest(index=index):self.changed(index,value)

    def test_stopped_status_requires_zero_pid(self):
        fields=inventory();fields[25]='false';fields[26]='0'
        self.invoke(fields,status=True,accepted=True);self.invoke(fields)
        fields[26]='1234';self.invoke(fields,status=True)
        fields[25]='true';fields[26]='0';self.invoke(fields,status=True)

    def test_pid_boundary(self):
        for value in ['','0','1','-1','+2','02','4194305','999999999999999999999999','2x']:
            with self.subTest(value=value):self.changed(26,value)
        for value in ['2','4194304']:
            fields=inventory();fields[26]=value;self.invoke(fields,accepted=True)

    def test_environment_overrides(self):
        for value in ['SHIM_STATE_DIR=/other','SHIM_API_ADDR=:80','LD_PRELOAD=/other',
                      'PATH=/tmp','TERM=other','container=docker','HOSTNAME=',
                      'HOSTNAME=bad/name','SSDR_RADIO_ADDRESS=127.0.0.1:4992',
                      'SSDR_RADIO_ADDRESS=999999999999999999999.0.0.1',
                      'SSDR_RADIO_ADDRESS=127.000.0.1']:
            with self.subTest(value=value):self.changed(33,encoded(value))
        self.changed(33,inventory()[29]);self.changed(27,'6')

    def test_hex_encoding(self):
        for value in ['f','zz','00','0a','41'*256]:
            with self.subTest(value=value):self.changed(33,value)
        self.changed(5,encoded('/flex-tailnet-shim').upper())

    def test_framing_and_bounds(self):
        good=('\n'.join(inventory())+'\n').encode()
        for value in [b'',good[:-1],good+b'\n',good+b'foreign\n',b'\0'+good,
                      good.replace(b'ENV',b'OTHER'),good.replace(b'END',b'OTHER'),
                      b'x'*8192+b'\n',good.replace(b'PDLEG001',b'PDLEG002')]:
            with self.subTest(length=len(value)):self.invoke(raw=value)

    def test_mount_path_is_one_fixed_overlay_layer(self):
        good=b'/var/lib/smartsdr/containers/storage/overlay/'+b'a'*64+b'/merged\n'
        bad=[good[:-1],good+b'\n',good.replace(b'/var/',b'/tmp/'),
             good.replace(b'a'*64,b'A'*64),good.replace(b'a'*64,b'a'*63),
             good.replace(b'a'*64,b'../'+b'a'*61),good.replace(b'/merged',b'/diff'),
             good.replace(b'/storage/',b'/storage//'),good+b'foreign\n',b'\0'+good]
        for data in [good,*bad]:
            with self.subTest(data=data):
                p=subprocess.run([str(CHECKER),'--mount-path'],input=data,capture_output=True,timeout=5)
                self.assertEqual(p.returncode,0 if data==good else 1)


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--checker',type=Path,required=True)
    a,rest=p.parse_known_args();CHECKER=a.checker.resolve()
    unittest.main(argv=['test_predecessor.py',*rest])
