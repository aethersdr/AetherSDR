#!/usr/bin/env python3
"""Fixed management-path resolution in disposable namespaces; no payload exec."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

CHECKER = None


class ManagedDirectoryTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='pd-managed-directory-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        for name in ['run/aetherpd/bin', 'fixture', 'dev', 'proc']:
            (self.root/name).mkdir(parents=True, mode=0o755)
        shutil.copyfile(CHECKER, self.root/'fixture/entry')
        (self.root/'fixture/entry').chmod(0o755)
        self.payload = self.root/'run/aetherpd/bin/flex-tailnet-shim'
        self.payload.write_bytes(b'\x7fELFfixed-runtime-payload');self.payload.chmod(0o755)
        sibling = self.root/'fixture/flex-tailnet-shim'
        sibling.write_bytes(b'\x7fELFwrong-sibling');sibling.chmod(0o755)

    def invoke(self, expected=0):
        p = subprocess.run(['bwrap', '--unshare-user', '--uid', '0', '--gid', '0', '--unshare-pid',
                            '--unshare-net', '--bind', str(self.root), '/', '--proc', '/proc', '--dev', '/dev',
                            '--die-with-parent', '--new-session', '--clearenv', '--chdir', '/fixture', '/fixture/entry'],
                           capture_output=True, timeout=5)
        self.assertEqual(p.returncode, expected, p.stderr)
        self.assertEqual(p.stdout, b'fixed-runtime-payload' if expected==0 else b'')

    def test_fixed_runtime_wins_over_executable_directory_and_cwd(self):
        self.invoke()

    def test_absent_payload_never_falls_back_to_sibling(self):
        self.payload.unlink();self.invoke(4)

    def test_each_writable_ancestor_refuses(self):
        for name in ['run', 'run/aetherpd', 'run/aetherpd/bin']:
            path = self.root/name;path.chmod(0o777);self.invoke(3);path.chmod(0o755)

    def test_directory_alias_refuses(self):
        path = self.root/'run/aetherpd/bin';path.rename(self.root/'elsewhere');path.symlink_to('/elsewhere')
        self.invoke(3)

    def test_payload_alias_and_multiple_links_refuse(self):
        self.payload.unlink();self.payload.symlink_to('/fixture/flex-tailnet-shim');self.invoke(4)
        self.payload.unlink();os.link(self.root/'fixture/flex-tailnet-shim', self.payload);self.invoke(4)

    def test_payload_modes_and_format_refuse(self):
        for mode in [0o777, 0o4755]:
            self.payload.chmod(mode);self.invoke(4)
        self.payload.chmod(0o755);self.payload.write_bytes(b'foreign');self.invoke(4)


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--checker', required=True, type=Path)
    args, rest = p.parse_known_args();CHECKER = args.checker.resolve()
    unittest.main(argv=[__file__, *rest])
