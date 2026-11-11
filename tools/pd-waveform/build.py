#!/usr/bin/env python3
"""Build AetherPD, the PureSignal-style predistortion waveform, as a legacy
waveform package holding two static binaries:

  aether-pd-armv7  for FLEX-6000 radios (Cortex-A8)
  aether-pd-arm64  for FLEX-8000 and Aurora radios (Cortex-A72), to develop
                   and test on; launch.sh picks one by `uname -m`.

    python3 -I build.py --output OUT_DIR [--host]

Needs zig (0.17). Only PureSignal's own WDSP files are compiled, with WDSP's
POSIX port and the vendored fftw3.h for its type names; no FFTW library is
linked: --gc-sections drops the functions that would call it. --host also
builds a native binary for the selftest.
"""
import argparse
import os
import pathlib
import re
import subprocess
import tempfile
import zipfile

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent.parent
WDSP = ROOT / 'third_party' / 'wdsp'
NAME = 'AetherPD'
FIXED_TIME = (2026, 1, 1, 0, 0, 0)
# A FLEX-6700's /user partition is 19 MB, shared by every legacy waveform.
MAX_BYTES = 512 * 1024
WDSP_TUS = ['calcc', 'iqc', 'delay', 'extrapolate', 'nurbs', 'nurbs_fit', 'nurbs_spline',
            'fir', 'utilities', 'impulse_cache']
ARMV7 = ['-target', 'arm-linux-musleabihf', '-mcpu=cortex_a8']
ARM64 = ['-target', 'aarch64-linux-musl']
CFLAGS = ['-O2', '-std=gnu11', '-ffunction-sections', '-fdata-sections', f'-ffile-prefix-map={ROOT}=.']
LDFLAGS = ['-static', '-s', '-Wl,--gc-sections', '-Wl,--build-id=none']
WDSP_INC = ['-include', str(WDSP / 'port' / 'include' / 'wdsp_port.h'),
            '-I', str(WDSP / 'upstream'), '-I', str(WDSP / 'port' / 'include'),
            '-I', str(WDSP / 'include'), '-I', str(ROOT / 'third_party' / 'fftw3' / 'include')]


def zig_cc(target, out, cache, static=True):
    env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(cache), ZIG_LOCAL_CACHE_DIR=str(cache))
    objs = []
    for tu in WDSP_TUS:
        o = out.parent / f'{out.name}-{tu}.o'
        subprocess.run(['zig', 'cc', *target, *CFLAGS, '-w', *WDSP_INC, '-c', '-o', str(o),
                        str(WDSP / 'upstream' / f'{tu}.c')], check=True, env=env)
        objs.append(str(o))
    o = out.parent / f'{out.name}-wdsp_port.o'
    subprocess.run(['zig', 'cc', *target, *CFLAGS, '-w', *WDSP_INC, '-c', '-o', str(o),
                    str(WDSP / 'port' / 'wdsp_port.c')], check=True, env=env)
    objs.append(str(o))
    o = out.parent / f'{out.name}-pd_core.o'
    subprocess.run(['zig', 'cc', *target, *CFLAGS, '-Wall', '-Wextra', '-Werror', *WDSP_INC, '-c', '-o', str(o),
                    str(HERE / 'pd_core.c')], check=True, env=env)
    objs.append(str(o))
    link = LDFLAGS if static else ['-Wl,--gc-sections']
    subprocess.run(['zig', 'cc', *target, *CFLAGS, *link, '-Wall', '-Wextra', '-Werror', '-o', str(out),
                    str(HERE / 'pd_waveform.c'), str(HERE / 'pd_align.c'), *objs, '-lm', '-lpthread'],
                   check=True, env=env)
    for o in objs:
        os.unlink(o)


def check_version():
    src = re.search(r'#define WF_VERSION "([^"]+)"', (HERE / 'pd_waveform.c').read_text())
    cfg = re.search(r'^Version: "([^"]+)"', (HERE / 'package' / f'{NAME}.cfg').read_text(), re.M)
    if not src or not cfg or src.group(1) != cfg.group(1):
        raise SystemExit(f'{NAME}.cfg Version and WF_VERSION in pd_waveform.c disagree')


def licenses():
    zig_env = subprocess.run(['zig', 'env'], capture_output=True, text=True, check=True).stdout
    lib_dir = re.search(r'"?lib_dir"?\s*[:=]\s*"([^"]+)"', zig_env).group(1)
    musl = pathlib.Path(lib_dir, 'libc', 'musl', 'COPYRIGHT').read_text(encoding='utf-8')
    return ('AetherPD is part of AetherSDR, licensed under the GNU GPL v3\n'
            '(https://github.com/aethersdr/AetherSDR). It includes PureSignal from WDSP\n'
            '(Warren Pratt NR0V; GPL v2 or later), whose source is in third_party/wdsp\n'
            'of the AetherSDR repository. The binaries are linked statically with musl\n'
            'libc, whose license follows.\n\n'
            + '=' * 64 + '\nmusl libc\n' + '=' * 64 + '\n' + musl)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--output', required=True, type=pathlib.Path)
    ap.add_argument('--host', action='store_true', help='also build a native binary for the selftest')
    ap.add_argument('--correction-off', action='store_true',
                    help='package with AETHER_PD_CORRECT=0, for the before half of a transmit test')
    a = ap.parse_args()
    check_version()
    a.output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='aether-pd-') as t:
        stage = pathlib.Path(t)
        cache = stage / 'zig-cache'
        files = {
            f'{NAME}.cfg': ((HERE / 'package' / f'{NAME}.cfg').read_bytes(), 0o644),
            'launch.sh': ((HERE / 'package' / 'launch.sh').read_bytes(), 0o755),
        }
        if a.correction_off:
            text = files['launch.sh'][0].decode()
            text = text.replace('export AETHER_PD_DAXIQ', 'AETHER_PD_CORRECT=0\nexport AETHER_PD_CORRECT AETHER_PD_DAXIQ', 1)
            files['launch.sh'] = (text.encode(), 0o755)
        for arch, target in (('armv7', ARMV7), ('arm64', ARM64)):
            zig_cc(target, stage / f'aether-pd-{arch}', cache)
            data = (stage / f'aether-pd-{arch}').read_bytes()
            files[f'aether-pd-{arch}'] = (data, 0o755)
            print(f'aether-pd-{arch}: {len(data)} bytes')
        files['LICENSES'] = (licenses().encode(), 0o644)
        target = a.output / (f'{NAME}-correction-off.ssdr_waveform' if a.correction_off else f'{NAME}.ssdr_waveform')
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
        if a.host:
            zig_cc(['-target', 'x86_64-linux-musl'], a.output / 'aether-pd-host', cache)
            print(f'built {a.output / "aether-pd-host"}')


if __name__ == '__main__':
    main()
