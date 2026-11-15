#!/usr/bin/env python3
"""Manual standalone-requester admission probe; never sends an installer or RF.

The caller supplies the reviewed public fixture's SHA256. Uses only the normal
waveform API, then removes the exact newly uploaded probe. No shim or SSH API.
"""
import argparse
import hashlib
import json
from pathlib import Path
import socket
import sys
import time
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'session'))
import kernel_probe_tool

NAME = 'AetherPDFirstInstallProbe'
PORT = 49183
kernel_probe_tool.NAME = NAME


class ObservedSocket:
    def __init__(self, channel):
        self.channel = channel
        self.pending = b''
        self.waveforms = []

    def __getattr__(self, name):
        return getattr(self.channel, name)

    def recv(self, size):
        data = self.channel.recv(size)
        self.pending += data
        if len(self.pending) > 65536:
            raise ValueError('Waveform observation exceeds bound')
        while b'\n' in self.pending:
            line, self.pending = self.pending.split(b'\n', 1)
            if line.startswith(b'S') and b'|waveform ' in line:
                status = line.split(b'|waveform ', 1)[1].decode('ascii').strip()
                self.waveforms.append(' '.join(x for x in status.split() if not x.startswith('env=')))
                if len(self.waveforms) > 256:
                    raise ValueError('Too many waveform observations')
        return data


def inventory(api):
    api.socket.waveforms.clear()
    api.command('sub waveform all')
    time.sleep(.2)
    api.command('sub waveform all')
    # FlexLib ParseWaveformStatus accepts native installed_list and container
    # records. Firmware can omit installed_list when no native waveform exists.
    observed = api.socket.waveforms
    if not any(x.startswith('wfp_status ') and 'ready=true' in x.split() for x in observed):
        raise RuntimeError('Waveform processor is not confirmed ready')
    return [x for x in observed if not x.startswith('wfp_status ')]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', required=True)
    parser.add_argument('--package', type=Path, required=True)
    parser.add_argument('--sha256', required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.output.exists():
        raise ValueError('Refusing to overwrite prior evidence')
    if args.package.is_symlink() or args.package.name != NAME + '.ssdr_waveform':
        raise ValueError('Expected named regular probe package')
    if not 0 < args.package.stat().st_size <= 256 * 1024:
        raise ValueError('Package exceeds the probe size bound')
    payload = args.package.read_bytes()
    if hashlib.sha256(payload).hexdigest() != args.sha256:
        raise ValueError('Package differs from reviewed SHA256')
    with zipfile.ZipFile(args.package) as archive:
        expected = {NAME + '/' + x for x in [NAME + '.cfg', 'pd-first-install-probe', 'pd-request', 'config', 'LICENSE']}
        if len(archive.infolist()) != len(expected) or set(archive.namelist()) != expected:
            raise ValueError('Unexpected package entries')
        config = archive.read(NAME + '/' + NAME + '.cfg').decode('ascii')
        if config.split('[setup]', 1)[1].split('[end]', 1)[0].strip():
            raise ValueError('Probe setup must be empty')
    report = {'package_sha256': args.sha256, 'installed_shim_used': False,
              'host_root_bridge_used': False, 'rf_commands': [], 'installation_attempted': False}
    api = kernel_probe_tool.Control(args.host)
    api.socket = ObservedSocket(api.socket)
    uploaded = False
    try:
        report['inventory_before'] = inventory(api)
        if any(NAME in x for x in report['inventory_before']):
            raise RuntimeError('Probe name already present')
        try:
            with socket.create_connection((args.host, PORT), .5):
                raise RuntimeError('Probe port already occupied')
        except (ConnectionRefusedError, TimeoutError):
            pass
        api.command('file filename ' + NAME + '.ssdr_waveform')
        reply = api.command(f'file upload {len(payload)} new_waveform')
        if reply and not reply.isdecimal():
            raise ValueError('Invalid upload port')
        port = int(reply) if reply else 42607
        if not 1024 <= port <= 65535:
            raise ValueError('Upload port outside range')
        time.sleep(.2)
        with socket.create_connection((args.host, port), 5) as upload:
            upload.settimeout(10)
            uploaded = True
            upload.sendall(payload)
        print('Uploaded standalone admission probe; waiting for report.', flush=True)
        deadline = time.monotonic() + 120
        while True:
            try:
                channel = socket.create_connection((args.host, PORT), .5)
                break
            except (ConnectionRefusedError, TimeoutError):
                if time.monotonic() >= deadline:
                    raise TimeoutError('Standalone probe report did not appear')
                time.sleep(.5)
        with channel:
            channel.settimeout(3)
            data = bytearray()
            while True:
                chunk = channel.recv(2049 - len(data))
                if not chunk:
                    break
                data.extend(chunk)
                if len(data) > 2048:
                    raise ValueError('Probe report exceeds bound')
        observation = json.loads(data)
        if observation.get('application') != NAME:
            raise ValueError('Unexpected report application')
        report['observation'] = observation
        print(json.dumps(observation, sort_keys=True), flush=True)
    except Exception as error:
        report['error'] = str(error)
        raise
    finally:
        try:
            if uploaded:
                api.command('waveform uninstall ' + NAME)
                report['uninstall_acknowledged'] = True
                deadline = time.monotonic() + 15
                while True:
                    report['inventory_after'] = inventory(api)
                    if not any(NAME in x for x in report['inventory_after']):
                        report['waveform_absent_after_uninstall'] = True
                        break
                    if time.monotonic() >= deadline:
                        raise TimeoutError('Probe remains in waveform inventory')
                    time.sleep(.5)
        except Exception as error:
            report['cleanup_error'] = str(error)
        finally:
            api.close()
            args.output.write_text(json.dumps(report, indent=2) + '\n')
    if report.get('cleanup_error'):
        raise RuntimeError(report['cleanup_error'])


if __name__ == '__main__':
    main()
