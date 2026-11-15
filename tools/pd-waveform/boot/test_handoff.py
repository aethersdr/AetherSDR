#!/usr/bin/env python3
"""Real exec/FD transfer with an inert verifier fixture; no radio access."""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import selectors
import shutil
import signal
import struct
import subprocess
import tempfile
import unittest
import bundle

CHECKER = None
SEED = hashlib.sha256(b'paired descriptor fixture key, not shipping').digest()


class HandoffTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='pd-handoff-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.control = self.root/'control'; self.control.mkdir(mode=0o700)
        for name in ['lock', 'other-lock']:
            (self.control/name).touch(mode=0o600)
        shutil.copyfile(CHECKER, self.root/'boot'); (self.root/'boot').chmod(0o755)
        raw = bytearray(4096); raw[:7] = b'\x7fELF\x02\x01\x01'
        struct.pack_into('<HH', raw, 16, 2, 183)
        self.members = {name: bytes(raw) if name != 'LICENSES' else b'fixture' for name in bundle.NAMES}
        self.payload, _ = bundle.assemble(self.members, 64, bytes(32), SEED)
        (self.root/'payload').write_bytes(self.payload); (self.root/'payload').chmod(0o644)

    def start(self, paired=True, fault='normal'):
        return subprocess.Popen([str(CHECKER), str(self.root), 'paired' if paired else 'legacy', fault],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            env=dict(os.environ, HANDOFF_TEST_SECRET='must-not-cross-exec'))

    def positive(self, paired=True, fault='normal', kill=False):
        process = self.start(paired, fault)
        try:
            with selectors.DefaultSelector() as selector:
                selector.register(process.stdout, selectors.EVENT_READ)
                self.assertTrue(selector.select(10), 'exec consumer did not become ready')
            line = process.stdout.readline()
            self.assertTrue(line, process.stderr.read().decode() if process.poll() is not None else 'empty ready line')
            self.assertEqual(json.loads(line), {'paired': int(paired), 'socket_owner': int(fault=='socket-owner'),
                                               'payload': bundle.blake(self.payload).hex()})
            with (self.control/'lock').open('rb') as lock:
                with self.assertRaises(BlockingIOError):
                    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            if kill:
                process.kill(); process.communicate(timeout=10)
                self.assertEqual(process.returncode, -signal.SIGKILL)
            else:
                stdout, stderr = process.communicate(input=b'\n', timeout=10)
                self.assertEqual(process.returncode, 0, stderr.decode()); self.assertEqual(stdout, b'')
            with (self.control/'lock').open('rb') as lock:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        finally:
            if process.poll() is None: process.kill()
            process.communicate(timeout=10)

    def refused(self, fault='normal'):
        process = self.start(fault=fault)
        stdout, stderr = process.communicate(input=b'\n', timeout=10)
        self.assertEqual(process.returncode, 1, stderr.decode()); self.assertEqual(stdout, b'')

    def test_actual_exec_retains_selection_lease_and_scrubs_environment(self):
        self.positive()

    def test_socket_owner_is_the_only_optional_argument(self):
        self.positive(fault='socket-owner')

    def test_legacy_exec_retains_lease_without_modern_marker(self):
        self.positive(paired=False)

    def test_consumer_death_releases_selection_lease(self):
        self.positive(kill=True)

    def test_wrong_payload_signature_and_content_refused(self):
        for offset in [0, bundle.PREFIX-1, len(self.payload)-1]:
            data = bytearray(self.payload); data[offset] ^= 1
            (self.root/'payload').write_bytes(data); self.refused()

    def test_writable_payload_descriptor_refused(self):
        self.refused(fault='writable-payload')

    def test_payload_metadata_and_hardlinks_refused(self):
        p = self.root/'payload'; p.chmod(0o666); self.refused(); p.chmod(0o644)
        os.link(p, self.root/'alias'); self.refused()

    def test_foreign_lease_and_nonempty_lock_refused(self):
        self.refused(fault='wrong-lease')
        (self.control/'lock').write_bytes(b'foreign'); self.refused()

    def test_executed_inode_must_match_inherited_boot(self):
        p = self.control/'wrong-self'
        shutil.copyfile(CHECKER, p); p.chmod(0o755)
        self.refused()


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--checker', type=Path, required=True)
    args, rest = parser.parse_known_args()
    CHECKER = args.checker.resolve()
    unittest.main(argv=[__file__, *rest])
