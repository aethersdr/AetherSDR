#!/usr/bin/env python3
"""Offline systemd ordering check. No service or radio process is executed."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
import units


@unittest.skipUnless(shutil.which('systemd-analyze'), 'requires Linux systemd verifier')
class UnitOrderingTest(unittest.TestCase):
    def test_boot_transaction_has_no_cycle(self):
        for arch,managed,restart in [('arm64',False,False),('arm64',True,False),('armv7',False,False),('armv7',True,False),('arm64',True,True),('armv7',True,True)]:
            with self.subTest(arch=arch,managed=managed,restart=restart), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                for name, text in units.render(arch,managed,control_restart=restart).items():
                    # This test checks ordering, not payload availability on the
                    # build host. Executable validation has separate native tests.
                    lines = []
                    for line in text.splitlines():
                        if line.startswith(('ExecStart=', 'ExecStartPost=')):
                            line = line.split('=', 1)[0] + '=' + shutil.which('true')
                        lines.append(line)
                    (root / name).write_text('\n'.join(lines) + '\n')
                (root / 'default.target').write_text(
                    '[Unit]\nRequires=basic.target aetherpd-shim.service\n'
                    'After=basic.target aetherpd-shim.service\n')
                env = dict(os.environ, SYSTEMD_UNIT_PATH=str(root) + ':/usr/lib/systemd/system')
                result = subprocess.run(
                    ['systemd-analyze', 'verify', '--man=no', '--generators=no', 'default.target'],
                    env=env, capture_output=True, text=True, timeout=20)
                self.assertEqual(result.returncode, 0, result.stderr)
                # systemd may return success after deleting jobs to break a cycle.
                self.assertNotIn('ordering cycle', result.stderr.lower(), result.stderr)


if __name__ == '__main__':
    unittest.main()
