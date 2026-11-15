#!/usr/bin/env python3
"""Exercise the native first-install broker and privileged config consumer."""
import argparse
import array
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import unittest

BROKER = None
CONSUMER = None


class FirstRequestTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='pd-first-auth-')
        self.addCleanup(self.temp.cleanup)
        self.record = b'PDFPUB01PDDELIV1' + struct.pack('>II', 64, 1) + os.urandom(96)
        self.path = Path(self.temp.name) / 'request'
        self.path.write_bytes(self.record)
        self.path.chmod(0o444)

    def broker(self, connections):
        process = subprocess.Popen([str(BROKER), str(self.path), str(connections)],
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        def cleanup():
            if process.poll() is None:
                process.kill()
            process.wait(timeout=3)
            process.stdout.close()
            process.stderr.close()
        self.addCleanup(cleanup)
        self.assertEqual(process.stdout.readline(), b'ready\n')
        return process

    def raw(self, request, rights=False):
        with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as client:
            client.settimeout(3)
            client.connect('\0aetherpd-config-' + self.record[88:].hex())
            ancillary = []
            if rights:
                ancillary = [(socket.SOL_SOCKET, socket.SCM_RIGHTS, array.array('i', [1]))]
            client.sendmsg([request], ancillary)
            data, control, flags, _ = client.recvmsg(9, socket.CMSG_SPACE(4))
            for level, kind, value in control:
                if level == socket.SOL_SOCKET and kind == socket.SCM_RIGHTS:
                    for fd in array.array('i', value):
                        os.close(fd)
            return data, len(control), flags

    def fetch(self, role, expected=0):
        result = subprocess.run([str(CONSUMER), str(self.path), role], capture_output=True, timeout=5)
        self.assertEqual(result.returncode, expected, result.stderr)
        self.assertTrue(json.loads(result.stdout)['no_descriptor_leak'])

    def test_roles_receipt_and_descriptor_rejection(self):
        process = self.broker(6)
        self.assertEqual(self.raw(b'PDCFGAC1' + self.record[:-1] + bytes([self.record[-1] ^ 1]))[:2], (b'', 0))
        self.assertEqual(self.raw(b'UNKNOWN1' + self.record)[:2], (b'', 0))
        self.assertEqual(self.raw(b'PDCFGAC1' + self.record, rights=True)[:2], (b'', 0))
        self.fetch('accept')
        self.fetch('accept', 3)
        self.fetch('deliver')
        self.assertEqual(process.wait(timeout=3), 0)

    def test_consumer_checks_public_record(self):
        process = self.broker(2)
        for offset in [0, 16, 23]:
            bad = bytearray(self.record)
            bad[offset] ^= 1
            self.path.chmod(0o600)
            self.path.write_bytes(bad)
            self.path.chmod(0o444)
            self.fetch('accept', 3)
        self.path.chmod(0o600)
        self.path.write_bytes(self.record)
        self.path.chmod(0o666)
        self.fetch('accept', 3)
        self.path.chmod(0o444)
        self.fetch('arbitrary-role', 3)
        self.fetch('deliver')
        self.fetch('accept')
        self.assertEqual(process.wait(timeout=3), 0)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--broker', type=Path, required=True)
    parser.add_argument('--consumer', type=Path, required=True)
    args, rest = parser.parse_known_args()
    BROKER, CONSUMER = args.broker.resolve(), args.consumer.resolve()
    unittest.main(argv=[__file__, *rest])
