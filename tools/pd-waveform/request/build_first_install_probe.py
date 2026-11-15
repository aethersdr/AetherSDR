#!/usr/bin/env python3
"""Build the public, non-authorizing native admission probe. Does not upload."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import zipfile
import build

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
NAME = 'AetherPDFirstInstallProbe'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    requester = build.build(output / 'native', arch='arm64', request=True)
    probe = output / 'native/pd-first-install-probe'
    subprocess.run(['zig', 'cc', '-target', 'aarch64-linux-musl', '-O2', '-static', '-s',
                    '-Wall', '-Wextra', '-Werror', str(HERE / 'probe_first_install.c'), '-o', str(probe)],
                   check=True, env=dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(output / 'cache'),
                                       ZIG_LOCAL_CACHE_DIR=str(output / 'cache')))
    cfg = f'''[header]
Name: {NAME}
Version: "0.1.1"
Minimum-SmartSDR-Version: 4.2.18.0
Author: AetherSDR
License: GPL-3.0
Executable: "probe.sh"

[setup]

[end]
'''.encode()
    launcher = b'#!/bin/sh\nset -eu\nexec "${0%/*}/pd-first-install-probe"\n'
    # Public fixture only: no corresponding carrier, signed worker or release.
    record = (b'PDXFER01' + hashlib.sha256(b'public no-authority first-install probe fixture').digest()
              + b'PDDELIV1' + struct.pack('>II', 64, 1)
              + hashlib.blake2b(b'no worker', digest_size=32).digest()
              + hashlib.blake2b(b'no plan', digest_size=32).digest())
    files = {NAME + '.cfg': (cfg, 0o644), 'probe.sh': (launcher, 0o755),
             'pd-first-install-probe': (probe.read_bytes(), 0o755),
             'pd-request': (requester.read_bytes(), 0o755), 'config': (record, 0o600),
             'LICENSE': ((ROOT / 'LICENSE').read_bytes(), 0o644)}
    target = output / (NAME + '.ssdr_waveform')
    with zipfile.ZipFile(target, 'w', zipfile.ZIP_DEFLATED) as archive:
        for name, (data, mode) in sorted(files.items()):
            entry = zipfile.ZipInfo(NAME + '/' + name, (2026, 1, 1, 0, 0, 0))
            entry.create_system = 3
            entry.external_attr = (0o100000 | mode) << 16
            entry.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(entry, data)
    result = {'sha256': hashlib.sha256(target.read_bytes()).hexdigest(),
              'bytes': target.stat().st_size, 'package': str(target),
              'contains_installation_authority': False}
    (output / 'package.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result))


if __name__ == '__main__':
    main()
