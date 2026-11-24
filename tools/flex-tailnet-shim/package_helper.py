#!/usr/bin/env python3
"""Package the discovery helper for a release: discovery-helper-<version>.zip.

The helper is published as a release asset beside the shim, at the shim's
version (#6984). The ZIP holds one discovery-helper/ directory with the
helper, its three launchers and a README, and is byte-for-byte
reproducible: fixed timestamps and modes, files in a fixed order.

    python3 -I package_helper.py --output OUT_DIR

native/build.py runs this too, so a waveform build leaves the helper's ZIP
beside the package.
"""
import argparse
import os
import pathlib
import re
import zipfile

HERE = pathlib.Path(__file__).resolve().parent
DIR = 'discovery-helper'
FIXED_TIME = (2026, 1, 1, 0, 0, 0)
# Name, mode. The .bat is stored with the CRLF line endings Git gives it.
FILES = (
    ('discovery_helper.py', 0o644),
    ('run-discovery-helper.bat', 0o644),
    ('run-discovery-helper.ps1', 0o644),
    ('run-discovery-helper.sh', 0o755),
)
README = """\
AetherSDR tailnet discovery helper {version}

Lists a radio on your tailnet in SmartSDR, SmartSDR DAX and SmartSDR CAT on
this computer. It connects to the radio's remote access (the tailnet shim)
and rebroadcasts the radio's announcement here.

Windows: run run-discovery-helper.bat. It needs Python 3.8 or newer from
python.org (not the Microsoft Store).
macOS and Linux: run ./run-discovery-helper.sh.

The radio's tailnet name defaults to flex-radio. Set TAILNET_SHIM to change
it, or pass --shim <name>.

The AetherSDR user guide, "Remote Access over Tailscale", explains when you
need it and what to tick in Remote Access.

Part of AetherSDR, licensed under the GNU GPL v3.
"""


def shim_version():
    m = re.search(r'^const shimVersion = "([^"]+)"', (HERE / 'main.go').read_text(), re.M)
    if not m:
        raise SystemExit('main.go has no shimVersion')
    return m.group(1)


def package(output):
    """Write the ZIP into output and return its path."""
    version = shim_version()
    entries = [(name, (HERE / name).read_bytes(), mode) for name, mode in FILES]
    entries.append(('README.txt', README.format(version=version).encode(), 0o644))
    output.mkdir(parents=True, exist_ok=True)
    target = output / f'{DIR}-{version}.zip'
    tmp = target.with_suffix('.tmp')
    with zipfile.ZipFile(tmp, 'w', zipfile.ZIP_DEFLATED) as z:
        for name, data, mode in entries:
            info = zipfile.ZipInfo(f'{DIR}/{name}', FIXED_TIME)
            info.create_system = 3
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = (0o100000 | mode) << 16
            z.writestr(info, data, compresslevel=9)
    os.replace(tmp, target)
    return target


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--output', required=True, type=pathlib.Path)
    a = ap.parse_args()
    target = package(a.output)
    print(f'built {target} ({target.stat().st_size} bytes)')


if __name__ == '__main__':
    main()
