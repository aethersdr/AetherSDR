#!/usr/bin/env python3
"""Build the AetherFT8 agent (agent.c, with the ft8_lib decoder from ft8_lib/)
for both kinds of radio, as static binaries with the licenses:

  AetherFT8.ssdr_waveform   a legacy waveform for FLEX-6000 radios (ARMv7),
                            which keep it on their /user partition.
  AetherFT8.tar.gz          a container image for FLEX-8000 and Aurora radios
                            (arm64), whose legacy waveforms have only RAM: a
                            container's writable layer keeps the settings
                            across a reboot.

    python3 -I build.py --output OUT_DIR [--host]

Needs zig 0.17 (it bundles the linker and musl for both targets). The packages
are reproducible byte for byte with the same zig. --host also builds a native
binary for the tests.

This mirrors tools/wspr-agent/build.py deliberately, so that the two agents are
packaged the same way. The difference is the decoder: ft8_lib needs only -lm,
because its FFT (kissfft) is vendored inside it, where wsprd needs an FFTW
stand-in and a translated OSD.

⚠ Build this on Linux. The WSPR agent's build.py is known to produce a package
the radio silently refuses when run on Windows, and this script inherits its
packaging, so assume the same until someone proves otherwise.
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
NAME = 'AetherFT8'
FIXED_TIME = (2026, 1, 1, 0, 0, 0)
# A FLEX-6700's /user partition is 19 MB, shared by every legacy waveform.
# (That radio cannot host this one -- its /user is full -- but the bound is
# the platform's, not one radio's.)
MAX_BYTES = 512 * 1024
# Decoder scratch stays in RAM: the writable layer is the radio's flash.
CONTAINER_ENV = ['TMPDIR=/dev/shm']

# ft8_lib's own sources. encode.c is deliberately absent: it is the
# transmit-side waveform encoder and is not vendored (ft8_lib/PROVENANCE.md),
# so there is no transmit code path to leave out of the link.
FT8_LIB = [
    'ft8/constants.c', 'ft8/crc.c', 'ft8/decode.c', 'ft8/ldpc.c',
    'ft8/message.c', 'ft8/text.c',
    'common/monitor.c', 'common/wave.c',
    'fft/kiss_fft.c', 'fft/kiss_fftr.c',
]

ARMV7 = ['-target', 'arm-linux-musleabihf', '-mcpu=cortex_a8']
ARM64 = ['-target', 'aarch64-linux-musl']
CFLAGS = ['-O2', '-std=gnu11', '-Wall', '-ffunction-sections', '-fdata-sections', f'-ffile-prefix-map={HERE}=.']
LDFLAGS = ['-static', '-s', '-Wl,--gc-sections', '-Wl,--build-id=none']


def zig_cc(args, out, cache):
    env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(cache), ZIG_LOCAL_CACHE_DIR=str(cache))
    objs = []
    # ft8_lib's sources are built as upstream ships them, so their warnings are
    # not ours to fix; agent.c and grayline.c are built strictly.
    for rel in FT8_LIB:
        o = out.parent / f'{out.name}-{pathlib.Path(rel).stem}.o'
        subprocess.run(['zig', 'cc', *args, *CFLAGS, '-w', '-I', str(HERE / 'ft8_lib'),
                        '-c', '-o', str(o), str(HERE / 'ft8_lib' / rel)],
                       check=True, env=env)
        objs.append(str(o))
    subprocess.run(['zig', 'cc', *args, *CFLAGS, *LDFLAGS, '-Wextra', '-Werror',
                    '-I', str(HERE), '-I', str(HERE / 'ft8_lib'), '-o', str(out),
                    str(HERE / 'agent.c'), str(HERE / 'grayline.c'), *objs, '-lm'],
                   check=True, env=env)
    for o in objs:
        os.unlink(o)


def check_version():
    agent = re.search(r'#define AGENT_VERSION "([^"]+)"', (HERE / 'agent.c').read_text(encoding='utf-8'))
    cfg = re.search(r'^Version: "([^"]+)"', (HERE / 'package' / f'{NAME}.cfg').read_text(encoding='utf-8'), re.M)
    if not agent or not cfg or agent.group(1) != cfg.group(1):
        raise SystemExit(f'{NAME}.cfg Version and AGENT_VERSION in agent.c disagree')


def check_version_zig():
    out = subprocess.run(['zig', 'version'], capture_output=True, text=True, check=True).stdout.strip()
    if not out.startswith('0.17'):
        raise SystemExit(f'needs zig 0.17, found {out}')


def licenses():
    zig_lib = subprocess.run(['zig', 'env'], capture_output=True, text=True, check=True).stdout
    lib_dir = re.search(r'"?lib_dir"?\s*[:=]\s*"([^"]+)"', zig_lib).group(1)
    musl = pathlib.Path(lib_dir, 'libc', 'musl', 'COPYRIGHT').read_text(encoding='utf-8')
    ft8 = (HERE / 'ft8_lib' / 'LICENSE').read_text(encoding='utf-8')
    return ('AetherFT8 is part of AetherSDR, licensed under the GNU GPL v3\n'
            '(https://github.com/aethersdr/AetherSDR). It includes the ft8_lib\n'
            'decoder (Karlis Goba YL3JG, MIT; https://github.com/kgoba/ft8_lib),\n'
            'whose source is in tools/ft8-agent/ft8_lib of the AetherSDR\n'
            'repository, with its transmit-side encoder removed. ft8_lib bundles\n'
            'kissfft (Mark Borgerding, BSD-3-Clause). The binaries are linked\n'
            'statically with musl libc, whose license follows.\n\n'
            + '=' * 64 + '\nft8_lib\n' + '=' * 64 + '\n' + ft8 + '\n'
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
    ap.add_argument('--armv7-only', action='store_true',
                    help='skip the arm64 container (needs flex-tailnet-shim/mkimage.py)')
    a = ap.parse_args()
    check_version_zig()
    check_version()
    a.output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='aether-ft8-') as t:
        stage = pathlib.Path(t)
        cache = stage / 'zig-cache'
        files = {
            f'{NAME}.cfg': ((HERE / 'package' / f'{NAME}.cfg').read_bytes(), 0o644),
            'launch.sh': ((HERE / 'package' / 'launch.sh').read_bytes(), 0o755),
        }
        zig_cc(ARMV7, stage / 'aether-ft8-armv7', cache)
        files['aether-ft8-armv7'] = ((stage / 'aether-ft8-armv7').read_bytes(), 0o755)
        license_text = licenses().encode()
        files['LICENSES'] = (license_text, 0o644)
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

        if not a.armv7_only:
            zig_cc(ARM64, stage / 'aether-ft8', cache)
            version = re.search(r'#define AGENT_VERSION "([^"]+)"', (HERE / 'agent.c').read_text(encoding='utf-8')).group(1)
            image = mkimage().build_image(NAME, [('aether-ft8', (stage / 'aether-ft8').read_bytes(), 0o755),
                                                 ('LICENSES', license_text, 0o644)], version, CONTAINER_ENV)
            if len(image) > MAX_BYTES:
                raise SystemExit(f'image is {len(image)} bytes, over the {MAX_BYTES}-byte bound')
            image_path = a.output / f'{NAME}.tar.gz'
            image_path.write_bytes(image)
            print(f'built {image_path} ({len(image)} bytes)')

        if a.host:
            zig_cc(['-target', 'x86_64-linux-musl'], a.output / 'aether-ft8-host', cache)
            print(f'built {a.output / "aether-ft8-host"}')


if __name__ == '__main__':
    main()
