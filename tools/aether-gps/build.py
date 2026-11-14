#!/usr/bin/env python3
"""Build the AetherGPS relay (agent.c) for both kinds of radio, as static
binaries with the licenses:

  AetherGPS.ssdr_waveform  a legacy waveform for FLEX-6000 radios (ARMv7).
  AetherGPS.tar.gz         a container image for FLEX-8000 and Aurora radios
                           (arm64). Containers there share the radio's
                           network, so the relay reaches gpsd on loopback
                           and serves the LAN as a legacy waveform would.

    python3 -I build.py --output OUT_DIR [--host]

Needs zig **0.17.0** exactly: 0.17.1 is known to generate different ARMv7
code (it does for AetherWSPR), so the FLEX-6000 package would not match its
pin. The packages are reproducible byte for byte with that zig and a Python
using stock zlib (see tools/wspr-agent/build.py for why zlib-ng differs).
--host also builds a native binary for the tests.

AetherSDR ships both as Qt resources: copy them to resources/waveforms/ and
update the version, SHA-256s and sizes in src/core/AetherGpsPackage.h
together.
"""
import argparse
import importlib.util
import os
import pathlib
import re
import subprocess
import tempfile
import zipfile

HERE = pathlib.Path(__file__).resolve().parent
NAME = 'AetherGPS'
FIXED_TIME = (2026, 1, 1, 0, 0, 0)
# A FLEX-6700's /user partition is 19 MB, shared by every legacy waveform.
MAX_BYTES = 256 * 1024
ARMV7 = ['-target', 'arm-linux-musleabihf', '-mcpu=cortex_a8']
ARM64 = ['-target', 'aarch64-linux-musl']
CFLAGS = ['-Os', '-static', '-s', '-std=c11', '-D_DEFAULT_SOURCE', '-Wall', '-Wextra',
          '-Werror', '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
          '-Wl,--build-id=none', f'-ffile-prefix-map={HERE}=.']


def zig_cc(args, out, cache):
    env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(cache), ZIG_LOCAL_CACHE_DIR=str(cache))
    subprocess.run(['zig', 'cc', *args, *CFLAGS, '-o', str(out), str(HERE / 'agent.c')],
                   check=True, env=env)


def version():
    agent = re.search(r'#define AGENT_VERSION "([^"]+)"', (HERE / 'agent.c').read_text())
    cfg = re.search(r'^Version: "([^"]+)"', (HERE / 'package' / f'{NAME}.cfg').read_text(), re.M)
    if not agent or not cfg or agent.group(1) != cfg.group(1):
        raise SystemExit(f'{NAME}.cfg Version and AGENT_VERSION in agent.c disagree')
    return agent.group(1)


def licenses():
    zig_lib = subprocess.run(['zig', 'env'], capture_output=True, text=True, check=True).stdout
    lib_dir = re.search(r'"?lib_dir"?\s*[:=]\s*"([^"]+)"', zig_lib).group(1)
    musl = pathlib.Path(lib_dir, 'libc', 'musl', 'COPYRIGHT').read_text(encoding='utf-8')
    return ('AetherGPS is part of AetherSDR, licensed under the GNU GPL v3\n'
            '(https://github.com/aethersdr/AetherSDR). The binaries are linked\n'
            'statically with musl libc, whose license follows.\n\n'
            + '=' * 64 + '\nmusl libc\n' + '=' * 64 + '\n' + musl)


def mkimage():
    spec = importlib.util.spec_from_file_location('mkimage', HERE.parent / 'flex-tailnet-shim' / 'mkimage.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--output', required=True, type=pathlib.Path)
    ap.add_argument('--host', action='store_true', help='also build a native binary for tests')
    a = ap.parse_args()
    ver = version()
    a.output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='aether-gps-') as t:
        stage = pathlib.Path(t)
        cache = stage / 'zig-cache'
        license_text = licenses().encode()

        zig_cc(ARMV7, stage / 'aether-gps-armv7', cache)
        files = {
            f'{NAME}.cfg': ((HERE / 'package' / f'{NAME}.cfg').read_bytes(), 0o644),
            'launch.sh': ((HERE / 'package' / 'launch.sh').read_bytes(), 0o755),
            'aether-gps-armv7': ((stage / 'aether-gps-armv7').read_bytes(), 0o755),
            'LICENSES': (license_text, 0o644),
        }
        target = a.output / f'{NAME}.ssdr_waveform'
        tmp = target.with_suffix('.tmp')
        with zipfile.ZipFile(tmp, 'w', zipfile.ZIP_DEFLATED) as z:
            for name, (data, mode) in files.items():
                info = zipfile.ZipInfo(f'{NAME}/{name}', FIXED_TIME)
                info.create_system = 3
                info.compress_type = zipfile.ZIP_DEFLATED
                info.external_attr = (0o100000 | mode) << 16
                z.writestr(info, data, compresslevel=9)
        size = tmp.stat().st_size
        if size > MAX_BYTES:
            tmp.unlink()
            raise SystemExit(f'package is {size} bytes, over the {MAX_BYTES}-byte bound')
        os.replace(tmp, target)
        print(f'built {target} ({size} bytes)')

        zig_cc(ARM64, stage / 'aether-gps', cache)
        image = mkimage().build_image(NAME, [('aether-gps', (stage / 'aether-gps').read_bytes(), 0o755),
                                             ('LICENSES', license_text, 0o644)], ver)
        if len(image) > MAX_BYTES:
            raise SystemExit(f'image is {len(image)} bytes, over the {MAX_BYTES}-byte bound')
        image_path = a.output / f'{NAME}.tar.gz'
        image_path.write_bytes(image)
        print(f'built {image_path} ({len(image)} bytes)')
        if a.host:
            zig_cc(['-target', 'x86_64-linux-musl'], a.output / 'aether-gps-host', cache)
            print(f'built {a.output / "aether-gps-host"}')


if __name__ == '__main__':
    main()
