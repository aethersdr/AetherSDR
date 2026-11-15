#!/usr/bin/env python3
"""Build a read-only native kernel probe; never installs it or grants privileges."""
import argparse
import hashlib
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import zipfile

HERE = Path(__file__).resolve().parent
NAME = 'AetherPDKernelProbe'
CFG = '''[header]
Name: AetherPDKernelProbe
Version: "0.1.2"
Minimum-SmartSDR-Version: 4.2.18.0
Author: AetherSDR
License: GPL-3.0
Executable: "probe.sh"

[setup]

[end]
'''
LAUNCHER = '''#!/bin/sh
set -eu
case "$0" in
  */*) probe_dir=${0%/*} ;;
  *) probe_dir=. ;;
esac
case "$(uname -m)" in
  aarch64|arm64) exec "$probe_dir/pd-kernel-probe-arm64" --serve ;;
  armv7l|armv7|arm) exec "$probe_dir/pd-kernel-probe-armv7" --serve ;;
  *) echo "AetherPD kernel probe: unsupported architecture" >&2; exit 2 ;;
esac
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    zig_env = subprocess.check_output(['zig', 'env'], text=True)
    library = re.search(r'"?lib_dir"?\s*[:=]\s*"([^"]+)"', zig_env)
    if library is None:
        raise SystemExit('could not locate Zig musl license')
    musl = (Path(library.group(1)) / 'libc/musl/COPYRIGHT').read_bytes()
    files = {
        NAME + '.cfg': (CFG.encode(), 0o644),
        'probe.sh': (LAUNCHER.encode(), 0o755),
        'LICENSE': ((HERE.parents[2] / 'LICENSE').read_bytes(), 0o644),
        'LICENSES': (b'AetherPDKernelProbe includes statically linked musl libc.\n\n' + musl, 0o644),
    }
    with tempfile.TemporaryDirectory(prefix='pd-kernel-probe-', dir=args.output) as temporary:
        for arch in ('armv7', 'arm64'):
            stage = Path(temporary) / arch
            subprocess.run([sys.executable, str(HERE / 'build.py'), '--output', str(stage),
                            '--arch', arch, '--probe-only'], check=True)
            name = 'pd-kernel-probe-' + arch
            files[name] = ((stage / name).read_bytes(), 0o755)
        target = args.output / (NAME + '.ssdr_waveform')
        pending = target.with_suffix('.tmp')
        with zipfile.ZipFile(pending, 'w', zipfile.ZIP_DEFLATED) as archive:
            for name, (data, mode) in sorted(files.items()):
                entry = zipfile.ZipInfo(NAME + '/' + name, (2026, 1, 1, 0, 0, 0))
                entry.create_system = 3
                entry.external_attr = (0o100000 | mode) << 16
                entry.compress_type = zipfile.ZIP_DEFLATED
                archive.writestr(entry, data, compresslevel=9)
        if pending.stat().st_size > 512 * 1024:
            pending.unlink()
            raise SystemExit('probe exceeds512KiB diagnostic package limit')
        pending.replace(target)
    print(target, target.stat().st_size, hashlib.sha256(target.read_bytes()).hexdigest())


if __name__ == '__main__':
    main()
