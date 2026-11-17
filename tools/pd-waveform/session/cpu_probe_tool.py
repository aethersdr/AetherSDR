#!/usr/bin/env python3
"""Install/read/remove only the bounded ordinary-user AetherPD CPU diagnostic."""
import argparse
import hashlib
import io
import json
from pathlib import Path
import re
import socket
import time
import zipfile

import kernel_probe_tool
from cpu_probe_package import CASES, CFG, LAUNCHER, LIMIT, NAME

PORT = 48999
MAX_REPORT_BYTES = 16384


class Control(kernel_probe_tool.Control):
    name = NAME


def package(path, digest):
    if path.name != NAME + '.ssdr_waveform' or path.is_symlink() or not path.is_file():
        raise ValueError('expected a regular named CPU diagnostic package')
    if not re.fullmatch(r'[0-9a-f]{64}', digest) or not 0 < path.stat().st_size <= LIMIT:
        raise ValueError('invalid reviewed checksum or package size')
    with path.open('rb') as channel:
        payload = channel.read(LIMIT + 1)
    if len(payload) > LIMIT or hashlib.sha256(payload).hexdigest() != digest:
        raise ValueError('CPU diagnostic checksum mismatch')
    expected = {NAME + '/' + name for name in [NAME + '.cfg', 'probe.sh', 'LICENSE', 'LICENSES',
                                              'pd-cpu-probe-armv7', 'pd-cpu-probe-arm64']}
    with zipfile.ZipFile(io.BytesIO(payload)) as archive:
        entries = archive.infolist()
        if len(entries) != len(expected) or {entry.filename for entry in entries} != expected:
            raise ValueError('unexpected or duplicate diagnostic package entry')
        if sum(entry.file_size for entry in entries) > 1024 * 1024:
            raise ValueError('expanded diagnostic exceeds1MiB')
        for entry in entries:
            executable = entry.filename.endswith(('probe.sh', 'probe-armv7', 'probe-arm64'))
            if entry.external_attr >> 16 != (0o100755 if executable else 0o100644):
                raise ValueError('unexpected diagnostic type/permissions')
        if archive.read(NAME + '/' + NAME + '.cfg') != CFG.encode() or archive.read(NAME + '/probe.sh') != LAUNCHER.encode():
            raise ValueError('diagnostic launcher/manifest differs from reviewed source')
        for arch, width, machine in [('armv7', 1, 40), ('arm64', 2, 183)]:
            binary = archive.read(NAME + '/pd-cpu-probe-' + arch)
            if len(binary) < 64 or binary[:4] != b'\x7fELF' or binary[4] != width or binary[5] != 1 or int.from_bytes(binary[18:20], 'little') != machine:
                raise ValueError('invalid diagnostic executable architecture')
    return payload


def report(data):
    if len(data) > MAX_REPORT_BYTES:
        raise ValueError('CPU diagnostic report exceeds bound')
    parsed = json.loads(data)
    if (not isinstance(parsed, dict) or parsed.get('application') != NAME
            or type(parsed.get('version')) is not int or parsed['version'] != 1
            or parsed.get('rf_commands') != [] or parsed.get('radio_api_commands') != []
            or type(parsed.get('passed')) is not bool or not isinstance(parsed.get('tests'), list)
            or len(parsed['tests']) != len(CASES)):
        raise ValueError('unexpected CPU diagnostic report')
    for name, result in zip(CASES, parsed['tests']):
        if (not isinstance(result, dict) or result.get('name') != name or type(result.get('passed')) is not bool
                or any(type(result.get(key)) is not int for key in ['exit', 'signal', 'errno', 'elapsed_ns', 'user_us', 'system_us', 'max_rss_kib'])
                or type(result.get('timed_out')) is not bool or type(result.get('output_truncated')) is not bool
                or not isinstance(result.get('output'), str) or len(result['output']) >= 1024
                or any(result[key] < 0 for key in ['signal', 'errno', 'elapsed_ns', 'user_us', 'system_us', 'max_rss_kib'])
                or result['passed'] != (result['exit'] == 0 and result['signal'] == 0 and result['errno'] == 0 and not result['timed_out'])):
            raise ValueError('invalid CPU diagnostic case')
    if parsed['passed'] != all(case['passed'] for case in parsed['tests']):
        raise ValueError('inconsistent CPU diagnostic summary')
    return parsed


def status(host):
    with socket.create_connection((host, PORT), 3) as channel:
        deadline = time.monotonic() + 3
        data = bytearray()
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError('CPU diagnostic report deadline exceeded')
            channel.settimeout(remaining)
            chunk = channel.recv(MAX_REPORT_BYTES + 1 - len(data))
            if not chunk:
                break
            data.extend(chunk)
            if len(data) > MAX_REPORT_BYTES:
                raise ValueError('CPU diagnostic report exceeds bound')
    return report(data)


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
            print('CPU diagnostic uninstall accepted; verify TCP48999 closes.')
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
            print(f'Uploaded {len(payload)} bytes; allow up to225s for the five cases before reading TCP48999.')
    finally:
        api.close()


if __name__ == '__main__':
    main()
