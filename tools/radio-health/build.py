#!/usr/bin/env python3
"""Build AetherHealth.ssdr_waveform: the AetherHealth agent (agent.c) as two
static binaries, ARMv7 for FLEX-6000 radios and arm64 for FLEX-8000/Aurora,
with the launcher that picks one, the waveform manifest and the licenses.

    python3 -I build.py --output OUT_DIR [--host]

Needs zig 0.17 (it bundles the linker and musl for both targets). The
package is reproducible byte for byte with the same zig. --host also builds
a native binary for the tests.

AetherSDR ships the package as a Qt resource: copy it to
resources/waveforms/AetherHealth.ssdr_waveform and update the version,
SHA-256 and size in src/core/RadioHealthPackage.h together.
"""
import argparse
import os
import pathlib
import re
import subprocess
import tempfile
import zipfile

HERE = pathlib.Path(__file__).resolve().parent
NAME = 'AetherHealth'
FIXED_TIME = (2026, 1, 1, 0, 0, 0)
# A FLEX-6700's /user partition is 19 MB, shared by every legacy waveform.
MAX_BYTES = 256 * 1024
TARGETS = {
    'aether-health-armv7': ['-target', 'arm-linux-musleabihf', '-mcpu=cortex_a8'],
    'aether-health-arm64': ['-target', 'aarch64-linux-musl'],
}
CFLAGS = ['-Os', '-static', '-s', '-std=c11', '-D_DEFAULT_SOURCE', '-Wall', '-Wextra',
          '-Werror', '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
          '-Wl,--build-id=none', f'-ffile-prefix-map={HERE}=.']


def zig_cc(args, out, cache):
    env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(cache), ZIG_LOCAL_CACHE_DIR=str(cache))
    subprocess.run(['zig', 'cc', *args, *CFLAGS, '-o', str(out), str(HERE / 'agent.c')],
                   check=True, env=env)


def check_version():
    agent = re.search(r'#define AGENT_VERSION "([^"]+)"', (HERE / 'agent.c').read_text())
    cfg = re.search(r'^Version: "([^"]+)"', (HERE / 'package' / f'{NAME}.cfg').read_text(), re.M)
    if not agent or not cfg or agent.group(1) != cfg.group(1):
        raise SystemExit(f'{NAME}.cfg Version and AGENT_VERSION in agent.c disagree')


def licenses():
    zig_lib = subprocess.run(['zig', 'env'], capture_output=True, text=True, check=True).stdout
    lib_dir = re.search(r'"?lib_dir"?\s*[:=]\s*"([^"]+)"', zig_lib).group(1)
    musl = pathlib.Path(lib_dir, 'libc', 'musl', 'COPYRIGHT').read_text()
    return ('AetherHealth is part of AetherSDR, licensed under the GNU GPL v3\n'
            '(https://github.com/aethersdr/AetherSDR). The binaries are linked\n'
            'statically with musl libc, whose license follows.\n\n'
            + '=' * 64 + '\nmusl libc\n' + '=' * 64 + '\n' + musl)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--output', required=True, type=pathlib.Path)
    ap.add_argument('--host', action='store_true', help='also build a native binary for tests')
    a = ap.parse_args()
    check_version()
    a.output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='aether-health-') as t:
        stage = pathlib.Path(t)
        cache = stage / 'zig-cache'
        files = {
            f'{NAME}.cfg': ((HERE / 'package' / f'{NAME}.cfg').read_bytes(), 0o644),
            'launch.sh': ((HERE / 'package' / 'launch.sh').read_bytes(), 0o755),
        }
        for name, target in TARGETS.items():
            zig_cc(target, stage / name, cache)
            files[name] = ((stage / name).read_bytes(), 0o755)
        files['LICENSES'] = (licenses().encode(), 0o644)
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
        sizes = ', '.join(f'{n} {len(files[n][0])} bytes' for n in TARGETS)
        print(f'built {target} ({size} bytes; {sizes})')
        if a.host:
            zig_cc(['-target', 'x86_64-linux-musl'], a.output / 'aether-health-host', cache)
            print(f'built {a.output / "aether-health-host"}')


if __name__ == '__main__':
    main()
