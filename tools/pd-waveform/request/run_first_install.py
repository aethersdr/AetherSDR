#!/usr/bin/env python3
"""Run one explicitly approved first-install qualification through the public API.

Consumes independently verified development artifacts and a private new token.
Never contacts an existing shim, SSH endpoint or diagnostics bridge. No RF verbs.
This qualification client is not a shipping publisher catalogue or desktop UI.
"""
import argparse
import hashlib
import json
import re
import socket
import stat
import time
import urllib.error
import urllib.request
from pathlib import Path
from first_install_package import verify_uploads


def upload_commands(name, size):
    if name.endswith('.ssdr_waveform'):
        return ['file filename '+name, f'file upload {size} new_waveform']
    if name.endswith('.tar.gz'):
        return [f'file upload {size} waveform_docker_image {name}']
    raise ValueError('unsupported first-install artifact type')


def removal_command(name):
    return ('waveform remove_container ' if name.startswith('aetherpd-first-') else 'waveform uninstall ') + name


class Radio:
    def __init__(self, host, commands):
        self.host, self.allowed = host, commands
        self.socket = socket.create_connection((host, 4992), 5)
        self.pending, self.sequence, self.observed = b'', 0, []

    def command(self, command):
        if command not in self.allowed:
            raise ValueError('command outside exact approved upload/removal set')
        self.sequence += 1
        self.socket.sendall(f'C{self.sequence}|{command}\n'.encode('ascii'))
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            if b'\n' not in self.pending:
                self.socket.settimeout(max(.01, deadline-time.monotonic()))
                data = self.socket.recv(16384)
                if not data:
                    raise ConnectionError('radio API closed')
                self.pending += data
                if len(self.pending) > 65536:
                    raise ValueError('radio response exceeds bound')
                continue
            line, self.pending = self.pending.split(b'\n', 1)
            if b'|waveform ' in line:
                self.observed.append(line.decode('ascii').strip())
                if len(self.observed) > 512:
                    self.observed = self.observed[-256:]
            if line.startswith(f'R{self.sequence}|'.encode()):
                fields = line.rstrip(b'\r').split(b'|', 2)
                if len(fields) != 3 or not re.fullmatch(b'[a-fA-F0-9]{1,8}', fields[1]) or int(fields[1], 16):
                    raise RuntimeError('radio rejected bounded operation')
                return fields[2].decode('ascii')
        raise TimeoutError('radio reply timeout')

    def inventory(self):
        self.observed.clear()
        self.command('sub waveform all')
        time.sleep(.2)
        self.command('sub waveform all')
        if not any('wfp_status ' in line and 'ready=true' in line for line in self.observed):
            raise RuntimeError('waveform processor is not ready')
        return list(self.observed)

    def upload(self, name, data):
        reply = ''
        for command in upload_commands(name, len(data)):
            reply = self.command(command)
        port = int(reply) if reply else 42607
        if not 1024 <= port <= 65535:
            raise ValueError('invalid upload port')
        time.sleep(.2)
        with socket.create_connection((self.host, port), 5) as channel:
            channel.settimeout(15)
            channel.sendall(data)


def status(host, token, path):
    req = urllib.request.Request('http://' + host + ':48992/v1/' + path,
                                 headers={'Authorization': 'Bearer ' + token})
    with urllib.request.urlopen(req, timeout=3) as response:
        return json.loads(response.read(65537))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--host', required=True)
    p.add_argument('--attempt', type=Path, required=True)
    p.add_argument('--approve-plan', required=True)
    p.add_argument('--output', type=Path, required=True)
    args = p.parse_args()
    if args.output.exists():
        raise ValueError('evidence already exists')
    meta = json.loads((args.attempt/'approval.json').read_text())
    plan = (args.attempt/'plan').read_bytes()
    if hashlib.blake2b(plan, digest_size=32).hexdigest() != args.approve_plan or args.approve_plan != meta['plan_blake2b_256']:
        raise ValueError('approval does not match this exact first-install plan')
    names = [meta['native_name'], meta['carrier_name']]
    if not re.fullmatch(r'AetherPDFirst[a-f0-9]{16}', names[0]) or not re.fullmatch(r'aetherpd-first-[a-f0-9]{16}', names[1]):
        raise ValueError('unexpected per-attempt waveform names')
    payloads = {}
    for leaf in [names[0]+'.ssdr_waveform', names[1]+'.tar.gz']:
        data = (args.attempt/leaf).read_bytes()
        if not 0 < len(data) <= 20*1024*1024 or hashlib.sha256(data).hexdigest() != meta['artifacts'][leaf]['sha256']:
            raise ValueError('artifact differs from approved build')
        payloads[leaf] = data
    token_path = args.attempt/'admin-token'
    bound_names = verify_uploads(*payloads.values(), plan)
    if list(bound_names) != names:
        raise ValueError('archive names differ from approved attempt')
    st = token_path.lstat()
    if not stat.S_ISREG(st.st_mode) or st.st_mode & 0o077 or st.st_nlink != 1:
        raise ValueError('new admin token must be a private regular file')
    token = token_path.read_text()
    if not re.fullmatch('[0-9a-f]{64}', token) or hashlib.sha256(token.encode()).digest() != plan[148:180]:
        raise ValueError('token does not match approved identity')
    commands = {'sub waveform all', 'info'}
    commands.update(removal_command(name) for name in names)
    for leaf, data in payloads.items():
        commands.update(upload_commands(leaf, len(data)))
    radio = Radio(args.host, commands)
    report = {'passed': False, 'plan_blake2b_256': args.approve_plan, 'rf_commands': [],
              'installed_shim_used_for_authorization': False, 'diagnostics_bridge_used_for_installation': False}
    attempted = []
    try:
        identity = radio.command('info')
        if meta['radio_serial'] not in identity or meta['firmware'] not in identity or 'FLEX-8600' not in identity:
            raise ValueError('live target differs from approved target')
        report['inventory_before'] = radio.inventory()
        if any(name in row for name in names for row in report['inventory_before']):
            raise ValueError('attempt waveform already present')
        try:
            with socket.create_connection((args.host, 48992), 1):
                raise RuntimeError('first-install requires the old shim to be absent')
        except ConnectionRefusedError:
            pass
        for (leaf, data), name in zip(payloads.items(), names):
            attempted.append(name)
            radio.upload(leaf, data)
            print('Uploaded '+name, flush=True)
            # This short-lived native helper does not register a DSP waveform.
            # Public inventory is not its readiness signal. The carrier's UID-
            # checked RAM handoff and final commitment validate native startup.
            if name == names[0]:
                time.sleep(3)
                continue
            deadline = time.monotonic()+60
            while True:
                inventory = radio.inventory()
                if any(name in row for row in inventory):
                    break
                if time.monotonic() >= deadline:
                    raise TimeoutError('uploaded waveform did not appear')
                time.sleep(.5)
        deadline = time.monotonic()+180
        while time.monotonic() < deadline:
            try:
                observed = status(args.host, token, 'aetherpd/installation')
                if observed.get('installation_committed'):
                    assert observed.get('first_install_plan_blake2b_256') == args.approve_plan
                    assert observed.get('selected_release_blake2b_256') == meta['release_blake2b_256']
                    assert observed.get('selected_release_key') == meta['release_key']
                    report['commit'] = observed
                    identity = status(args.host, token, 'status')
                    assert identity['installation_id'] == meta['installation_id']
                    assert identity['state'] == 'local' and identity['provisioned'] and not identity['claimable']
                    report['identity'] = identity
                    report['passed'] = True
                    break
            except (OSError, ValueError):
                pass
            time.sleep(.5)
        if not report['passed']:
            raise TimeoutError('fresh transaction did not report committed')
    except Exception as error:
        report['error'] = type(error).__name__+': '+str(error)
    finally:
        report['removals'] = {}
        for name in reversed(attempted):
            try:
                radio.command(removal_command(name))
                report['removals'][name] = 'acknowledged'
            except Exception as error:
                report['removals'][name] = type(error).__name__
        try:
            deadline = time.monotonic()+20
            while True:
                report['inventory_after'] = radio.inventory()
                if not any(name in row for name in attempted for row in report['inventory_after']):
                    report['temporary_waveforms_absent'] = True
                    break
                if time.monotonic() >= deadline:
                    report['temporary_waveforms_absent'] = False
                    break
                time.sleep(.5)
        finally:
            radio.socket.close()
            args.output.write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps({k:report[k] for k in ['passed','error','temporary_waveforms_absent'] if k in report}), flush=True)
    if not report['passed'] or not report.get('temporary_waveforms_absent'):
        raise SystemExit(1)


if __name__ == '__main__':
    main()
