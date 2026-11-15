"""Socket-free package and transport-boundary checks for our probe tool."""
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import zipfile

import kernel_probe_package as builder
import kernel_probe_tool as tool


class Channel:
    def __init__(self, chunks):
        self.chunks = list(chunks)
        self.sent = []

    def __enter__(self):
        return self

    def __exit__(self, *args):
        pass

    def recv(self, size):
        return self.chunks.pop(0) if self.chunks else b''

    def sendall(self, payload):
        self.sent.append(payload)

    def settimeout(self, seconds):
        pass


class ProbeToolTest(unittest.TestCase):
    def test_fixed_control_commands_and_exact_ack(self):
        control = tool.Control.__new__(tool.Control)
        control.sequence, control.buffer = 0, b''
        control.socket = Channel([b'H123\nR1|0|', b'42607\n'])
        self.assertEqual(control.command('file upload 123 new_waveform'), '42607')
        self.assertEqual(control.socket.sent, [b'C1|file upload 123 new_waveform\n'])
        for text in ['xmit 1', 'waveform uninstall Other', 'file upload 0 new_waveform',
                     'waveform uninstall AetherPDKernelProbe\nC2|xmit 1']:
            with self.assertRaises(ValueError):
                control.command(text)
        self.assertEqual(len(control.socket.sent), 1)

    def test_malformed_and_oversized_control_reply(self):
        for payload in [b'R1|nothex|x\n', b'R1|50000001|denied\n', b'A' * 32769]:
            control = tool.Control.__new__(tool.Control)
            control.sequence, control.buffer = 0, b''
            control.socket = Channel([payload])
            with self.assertRaises((ValueError, RuntimeError)):
                control.command('waveform uninstall AetherPDKernelProbe')

    def test_report_bound_identity_and_boolean_version(self):
        valid = {'application': tool.NAME, 'version': 1, 'rf_commands': []}
        with patch.object(tool.socket, 'create_connection', return_value=Channel([json.dumps(valid).encode()])):
            self.assertEqual(tool.status('unused'), valid)
        bad = [b'A' * 2049, b'{}', json.dumps({**valid, 'version': True}).encode(),
               json.dumps({**valid, 'application': 'Other'}).encode()]
        for payload in bad:
            with patch.object(tool.socket, 'create_connection', return_value=Channel([payload])):
                with self.assertRaises(ValueError):
                    tool.status('unused')

    def test_report_total_deadline(self):
        with patch.object(tool.socket, 'create_connection', return_value=Channel([b'{'])):
            with patch.object(tool.time, 'monotonic', side_effect=[0, 1, 4]):
                with self.assertRaises(TimeoutError):
                    tool.status('unused')

    def make_package(self, folder, modify=None):
        files = {tool.NAME + '.cfg': (builder.CFG.encode(), 0o100644),
                 'probe.sh': (builder.LAUNCHER.encode(), 0o100755),
                 'LICENSE': (b'license', 0o100644), 'LICENSES': (b'notices', 0o100644)}
        for arch, machine in [('armv7', 40), ('arm64', 183)]:
            elf = bytearray(64)
            elf[:4], elf[5], elf[18:20] = b'\x7fELF', 1, machine.to_bytes(2, 'little')
            files['pd-kernel-probe-' + arch] = (bytes(elf), 0o100755)
        if modify:
            modify(files)
        path = folder / (tool.NAME + '.ssdr_waveform')
        with zipfile.ZipFile(path, 'w') as archive:
            for name, (data, mode) in files.items():
                entry = zipfile.ZipInfo(tool.NAME + '/' + name)
                entry.external_attr = mode << 16
                archive.writestr(entry, data)
        return path, hashlib.sha256(path.read_bytes()).hexdigest()

    def test_package_integrity_shape_modes_and_no_setup(self):
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            path, digest = self.make_package(folder)
            self.assertEqual(tool.package(path, digest), path.read_bytes())
            with self.assertRaises(ValueError):
                tool.package(path, '0' * 64)
            for change in [
                lambda f: f.update({'../escape': (b'x', 0o100644)}),
                lambda f: f.update({'probe.sh': (b'exec arbitrary', 0o100755)}),
                lambda f: f.update({'probe.sh': (builder.LAUNCHER.encode(), 0o104755)}),
                lambda f: f.update({tool.NAME + '.cfg': (builder.CFG.replace('[setup]', '[setup]\nxmit 1').encode(), 0o100644)}),
                lambda f: f.update({'pd-kernel-probe-armv7': (b'badELF', 0o100755)}),
            ]:
                path, digest = self.make_package(folder, change)
                with self.assertRaises(ValueError):
                    tool.package(path, digest)


if __name__ == '__main__':
    unittest.main()
