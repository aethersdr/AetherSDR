#!/usr/bin/env python3
"""Install, restart or remove AetherRadioProbe on a radio over its LAN API,
and fetch its report. The install is the legacy waveform upload FlexLib
itself uses (Radio.cs: `file filename`, `file upload <n> new_waveform`, then
the bytes on the port the radio returns, 42607 if it names none).

    python3 -I upload.py --host 192.168.1.20 --install AetherRadioProbe.ssdr_waveform
    python3 -I upload.py --host 192.168.1.20 --report report-boot1.json
    python3 -I upload.py --host 192.168.1.20 --uninstall

Some radios list a legacy waveform only after a reboot. SmartSDR's own
Waveforms window installs the same file just as well.
"""
import argparse
import pathlib
import socket
import sys
import time
import urllib.request

NAME = 'AetherRadioProbe'
REPORT_PORT = 48994


class Api:
    def __init__(self, host):
        self.host = host
        self.sock = socket.create_connection((host, 4992), 5)
        self.sock.settimeout(15)
        self.buf = b''
        self.seq = 0
        while not self.line().startswith('H'):
            pass

    def line(self):
        while b'\n' not in self.buf:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnectionError('radio closed the API connection')
            self.buf += chunk
        l, _, self.buf = self.buf.partition(b'\n')
        return l.decode(errors='replace').strip()

    def command(self, cmd):
        self.seq += 1
        self.sock.sendall(f'C{self.seq}|{cmd}\n'.encode())
        want = f'R{self.seq}|'
        while True:
            l = self.line()
            if l.startswith(want):
                _, status, *rest = l.split('|', 2)
                if int(status, 16) != 0:
                    raise RuntimeError(f'{cmd!r} failed: {l}')
                return rest[0] if rest else ''


def install(host, pkg):
    data = pkg.read_bytes()
    api = Api(host)
    api.command(f'file filename {pkg.name}')
    reply = api.command(f'file upload {len(data)} new_waveform')
    port = int(reply) if reply.strip().isdigit() else 42607
    time.sleep(0.2)
    with socket.create_connection((host, port), 5) as up:
        up.settimeout(60)
        up.sendall(data)
    print(f'sent {len(data)} bytes on port {port}; the radio installs it in the background')
    print('if it is not listed after a few minutes, reboot the radio')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--host', required=True)
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument('--install', type=pathlib.Path, metavar='PACKAGE')
    g.add_argument('--restart', action='store_true')
    g.add_argument('--uninstall', action='store_true')
    g.add_argument('--report', type=pathlib.Path, metavar='OUT_JSON')
    g.add_argument('--cleanup', action='store_true', help='remove the probe\'s marker files and stop it')
    a = ap.parse_args()
    if a.install:
        if a.install.name != f'{NAME}.ssdr_waveform':
            sys.exit(f'expected {NAME}.ssdr_waveform')
        install(a.host, a.install)
    elif a.restart:
        Api(a.host).command(f'waveform restart {NAME}')
    elif a.uninstall:
        Api(a.host).command(f'waveform uninstall {NAME}')
    elif a.report:
        with urllib.request.urlopen(f'http://{a.host}:{REPORT_PORT}/', timeout=10) as r:
            a.report.write_bytes(r.read())
        print(f'saved {a.report}')
    elif a.cleanup:
        req = urllib.request.Request(f'http://{a.host}:{REPORT_PORT}/cleanup', method='POST')
        with urllib.request.urlopen(req, timeout=10) as r:
            print(r.read().decode())


if __name__ == '__main__':
    main()
