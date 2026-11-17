#!/usr/bin/env python3
"""Install/read/remove only AetherPDKernelProbe through the ordinary waveform API.

FlexLib Radio.cs defines file filename/file upload new_waveform, fallback
port42607 and waveform uninstall. No RF, reboot, root or arbitrary command API.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import socket
import time
import zipfile

NAME = 'AetherPDKernelProbe'
PORT = 48998
LIMIT = 512 * 1024
MAX_REPORT_BYTES = 4096


class Control:
    def __init__(self, host):
        self.socket = socket.create_connection((host, 4992), 5)
        self.sequence = 0
        self.buffer = b''
        self.inventory = None

    def command(self, text):
        allowed = (text == 'sub waveform all'
                   or text == 'file filename ' + NAME + '.ssdr_waveform'
                   or text == 'waveform uninstall ' + NAME
                   or re.fullmatch(r'file upload [1-9][0-9]{0,5} new_waveform', text))
        if not allowed:
            raise ValueError('not a fixed probe upload/removal command')
        self.sequence += 1
        sequence = self.sequence
        self.socket.sendall(f'C{sequence}|{text}\n'.encode('ascii'))
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            if b'\n' not in self.buffer:
                self.socket.settimeout(max(.01, deadline - time.monotonic()))
                data = self.socket.recv(16384)
                if not data:
                    raise ConnectionError('radio API disconnected')
                self.buffer += data
                if len(self.buffer) > 32768:
                    raise ValueError('radio API input exceeded bound')
                continue
            line, self.buffer = self.buffer.split(b'\n', 1)
            if line.startswith(b'S') and b'|waveform installed_list=' in line:
                self.inventory = line.split(b'|waveform installed_list=', 1)[1].rstrip(b'\r').decode('ascii')
            if not line.startswith(f'R{sequence}|'.encode()):
                continue
            fields = line.rstrip(b'\r').split(b'|', 2)
            if len(fields) != 3 or not re.fullmatch(rb'[0-9A-Fa-f]{1,8}', fields[1]):
                raise ValueError('malformed command reply')
            if int(fields[1], 16):
                raise RuntimeError('radio rejected probe command: ' + fields[1].decode())
            return fields[2].decode('ascii')
        raise TimeoutError('probe command reply timeout')

    def close(self):
        self.socket.close()


def package(path, digest):
    if path.name != NAME + '.ssdr_waveform' or path.is_symlink() or not path.is_file():
        raise ValueError('expected a regular named probe package')
    if not re.fullmatch(r'[0-9a-f]{64}', digest):
        raise ValueError('expected reviewed lowercase SHA256')
    if not 0 < path.stat().st_size <= LIMIT:
        raise ValueError('probe package exceeds bound')
    payload = path.read_bytes()
    if hashlib.sha256(payload).hexdigest() != digest:
        raise ValueError('probe package checksum mismatch')
    expected = {NAME + '/' + n for n in [NAME + '.cfg', 'probe.sh', 'LICENSE', 'LICENSES',
                                       'pd-kernel-probe-armv7', 'pd-kernel-probe-arm64']}
    with zipfile.ZipFile(path) as archive:
        entries = archive.infolist()
        if len(entries) != len(expected) or {i.filename for i in entries} != expected:
            raise ValueError('unexpected or duplicate probe package entry')
        if sum(i.file_size for i in entries) > 1024 * 1024:
            raise ValueError('expanded probe package exceeds bound')
        for info in entries:
            executable = info.filename.endswith(('probe.sh', 'probe-armv7', 'probe-arm64'))
            if info.external_attr >> 16 != (0o100755 if executable else 0o100644):
                raise ValueError('unexpected probe file type/permissions')
        cfg = archive.read(NAME + '/' + NAME + '.cfg').decode('ascii')
        from kernel_probe_package import CFG, LAUNCHER
        if cfg != CFG or archive.read(NAME + '/probe.sh') != LAUNCHER.encode():
            raise ValueError('probe manifest/launcher does not match reviewed source')
        for arch, machine in [('armv7', 40), ('arm64', 183)]:
            elf = archive.read(NAME + '/pd-kernel-probe-' + arch)
            if len(elf) < 64 or elf[:4] != b'\x7fELF' or elf[5] != 1 or int.from_bytes(elf[18:20], 'little') != machine:
                raise ValueError('invalid native probe executable')
    return payload


def status(host):
    with socket.create_connection((host, PORT), 3) as channel:
        deadline = time.monotonic() + 3
        data = bytearray()
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError('probe report deadline exceeded')
            channel.settimeout(remaining)
            chunk = channel.recv(MAX_REPORT_BYTES + 1 - len(data))
            if not chunk:
                break
            data.extend(chunk)
            if len(data) > MAX_REPORT_BYTES:
                raise ValueError('probe report exceeds bound')
    report = json.loads(data)
    if (not isinstance(report, dict) or report.get('application') != NAME
            or type(report.get('version')) is not int or report.get('version') != 1
            or report.get('rf_commands') != []):
        raise ValueError('unexpected probe report')
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', required=True)
    sub = parser.add_subparsers(dest='action', required=True)
    install = sub.add_parser('install')
    install.add_argument('package', type=Path)
    install.add_argument('--sha256', required=True)
    sub.add_parser('status')
    sub.add_parser('uninstall')
    args = parser.parse_args()
    if args.action == 'status':
        print(json.dumps(status(args.host), indent=2))
        return
    payload = package(args.package, args.sha256) if args.action == 'install' else None
    api = Control(args.host)
    try:
        if payload is None:
            api.command('waveform uninstall ' + NAME)
            print('Probe uninstall accepted; verify its port closes.')
        else:
            api.command('file filename ' + NAME + '.ssdr_waveform')
            reply = api.command(f'file upload {len(payload)} new_waveform')
            if reply and not reply.isdecimal():
                raise ValueError('invalid waveform upload port')
            port = int(reply) if reply else 42607
            if not 1024 <= port <= 65535:
                raise ValueError('upload port outside range')
            time.sleep(.2)
            with socket.create_connection((args.host, port), 5) as upload:
                upload.settimeout(20)
                upload.sendall(payload)
            print(f'Uploaded {len(payload)} bytes; poll status after the radio starts the probe.')
    finally:
        api.close()


if __name__ == '__main__':
    main()
