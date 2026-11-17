import copy
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import zipfile

HERE = Path(__file__).resolve().parent
import sys
sys.path.insert(0, str(HERE))
import cpu_probe_tool as tool
from cpu_probe_package import CASES, NAME
from test_kernel_probe_tool import Channel


class CpuProbeTests(unittest.TestCase):
    def good(self):
        return {'application': NAME, 'version': 1, 'passed': True, 'rf_commands': [], 'radio_api_commands': [],
                'tests': [{'name': name, 'passed': True, 'exit': 0, 'signal': 0, 'errno': 0,
                           'elapsed_ns': 1, 'user_us': 1, 'system_us': 0, 'max_rss_kib': 0,
                           'timed_out': False, 'output_truncated': False, 'output': 'PASS'} for name in CASES]}

    def test_summary_and_case_failures_are_preserved(self):
        good = self.good()
        self.assertTrue(tool.report(json.dumps(good).encode())['passed'])
        failed = copy.deepcopy(good)
        failed['passed'] = False
        failed['tests'][2].update(passed=False, exit=-1, signal=9, timed_out=True)
        self.assertFalse(tool.report(json.dumps(failed).encode())['passed'])
        failed['passed'] = True
        with self.assertRaises(ValueError):
            tool.report(json.dumps(failed).encode())

    def test_malformed_oversize_and_wrong_contract_refused(self):
        for change in [lambda d: d.update(version=True), lambda d: d.update(rf_commands=['transmit']),
                       lambda d: d['tests'].pop(), lambda d: d['tests'][0].update(name='../command'),
                       lambda d: d['tests'][0].update(passed=True, exit=7),
                       lambda d: d['tests'][0].update(max_rss_kib=-1),
                       lambda d: d['tests'][0].update(user_us=True),
                       lambda d: d['tests'][0].update(output='x' * 1024)]:
            data = self.good()
            change(data)
            with self.assertRaises(ValueError):
                tool.report(json.dumps(data).encode())
        with self.assertRaises(ValueError):
            tool.report(b' ' * (tool.MAX_REPORT_BYTES + 1))

    def test_control_refuses_arbitrary_names_before_send(self):
        control = object.__new__(tool.Control)
        for command in ['transmit set mox=1', 'waveform uninstall AetherPD',
                        'waveform uninstall AetherPDKernelProbe', 'file filename other.ssdr_waveform',
                        'file upload 0 new_waveform', 'reboot']:
            with self.assertRaises(ValueError):
                control.command(command)

    def test_fixed_upload_and_uninstall_commands(self):
        control = object.__new__(tool.Control)
        control.sequence, control.buffer = 0, b''
        control.socket = Channel([b'R1|0|42607\n'])
        self.assertEqual(control.command('file filename ' + NAME + '.ssdr_waveform'), '42607')
        self.assertEqual(control.socket.sent, [('C1|file filename ' + NAME + '.ssdr_waveform\n').encode()])

    def make_package(self, folder, change=None):
        files = {NAME + '.cfg': (tool.CFG.encode(), 0o100644), 'probe.sh': (tool.LAUNCHER.encode(), 0o100755),
                 'LICENSE': (b'license', 0o100644), 'LICENSES': (b'notices', 0o100644)}
        for arch, width, machine in [('armv7', 1, 40), ('arm64', 2, 183)]:
            binary = bytearray(64)
            binary[:4], binary[4], binary[5], binary[18:20] = b'\x7fELF', width, 1, machine.to_bytes(2, 'little')
            files['pd-cpu-probe-' + arch] = (bytes(binary), 0o100755)
        if change:
            change(files)
        path = folder / (NAME + '.ssdr_waveform')
        with zipfile.ZipFile(path, 'w') as archive:
            for name, (data, mode) in files.items():
                entry = zipfile.ZipInfo(NAME + '/' + name)
                entry.external_attr = mode << 16
                archive.writestr(entry, data)
        return path, hashlib.sha256(path.read_bytes()).hexdigest()

    def test_package_shape_permissions_manifest_and_digest(self):
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            path, digest = self.make_package(folder)
            self.assertEqual(tool.package(path, digest), path.read_bytes())
            with self.assertRaises(ValueError):
                tool.package(path, '0' * 64)
            for change in [lambda f: f.update({'../escape': (b'x', 0o100644)}),
                           lambda f: f.update({'probe.sh': (b'exec other', 0o100755)}),
                           lambda f: f.update({'probe.sh': (tool.LAUNCHER.encode(), 0o104755)}),
                           lambda f: f.update({NAME + '.cfg': (tool.CFG.replace('[setup]', '[setup]\nxmit 1').encode(), 0o100644)}),
                           lambda f: f.update({'pd-cpu-probe-armv7': (b'badELF', 0o100755)})]:
                path, digest = self.make_package(folder, change)
                with self.assertRaises(ValueError):
                    tool.package(path, digest)

    def test_status_total_deadline_and_report_bound(self):
        encoded = json.dumps(self.good()).encode()
        with patch.object(tool.socket, 'create_connection', return_value=Channel([encoded])):
            self.assertTrue(tool.status('unused')['passed'])
        with patch.object(tool.socket, 'create_connection', return_value=Channel([b'{'])):
            with patch.object(tool.time, 'monotonic', side_effect=[0, 1, 4]):
                with self.assertRaises(TimeoutError):
                    tool.status('unused')
        with patch.object(tool.socket, 'create_connection', return_value=Channel([b'x' * (tool.MAX_REPORT_BYTES + 1)])):
            with self.assertRaises(ValueError):
                tool.status('unused')

    def test_package_checks_the_same_bounded_snapshot_it_returns(self):
        with tempfile.TemporaryDirectory() as temporary:
            path, digest = self.make_package(Path(temporary))
            expected = path.read_bytes()
            original = Path.open

            class ChangedAfterRead:
                def __enter__(self):
                    self.channel = original(path, 'rb')
                    return self.channel

                def __exit__(self, *args):
                    self.channel.close()
                    with original(path, 'wb') as changed:
                        changed.write(b'changed after snapshot')

            def opened(target, *args, **kwargs):
                if target == path and args == ('rb',):
                    return ChangedAfterRead()
                return original(target, *args, **kwargs)

            with patch.object(Path, 'open', opened):
                self.assertEqual(tool.package(path, digest), expected)


if __name__ == '__main__':
    unittest.main()
