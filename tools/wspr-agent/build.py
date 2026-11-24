#!/usr/bin/env python3
"""Build the AetherWSPR agent (agent.c, with the wsprd decoder from wsprd/)
for both kinds of radio, as static binaries with the licenses:

  AetherWSPR.ssdr_waveform  a legacy waveform for FLEX-6000 radios (ARMv7),
                            which keep it on their /user partition.
  AetherWSPR.tar.gz         a container image for FLEX-8000 and Aurora radios
                            (arm64), whose legacy waveforms have only RAM: a
                            container's writable layer keeps the settings
                            across a reboot.

    python3 -I build.py --output OUT_DIR [--host]

Needs zig **0.17.0** exactly -- not 0.17.1, which generates an ARMv7 binary
32 bytes larger from identical source (372760 -> 372792, measured on Linux),
so the FLEX-6000 package will not match its pin. The arm64 container is NOT
affected: it builds byte-identical under both. --host also builds a native
binary for the tests.

One more thing the packages are reproducible ACROSS only if it matches: the
zlib that Python's zipfile uses. CPython 3.14 bundles zlib-ng, which deflates
identical input to different bytes than stock zlib -- enough to change the
package sha without changing a single member byte. The pins are built with
**stock zlib**, which a Linux distribution Python has; a zlib-ng Python, such
as python.org's Windows CPython 3.14, will not match. The zlib patch level
does not matter: 1.3 and 1.3.2 give the same bytes.

AetherSDR ships both as Qt resources: copy them to resources/waveforms/ and
update the version, SHA-256s and sizes in src/core/WsprAgent.h together.
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
NAME = 'AetherWSPR'
FIXED_TIME = (2026, 1, 1, 0, 0, 0)
# A FLEX-6700's /user partition is 19 MB, shared by every legacy waveform.
MAX_BYTES = 512 * 1024
# Decoder scratch stays in RAM: the writable layer is the radio's flash.
CONTAINER_ENV = ['TMPDIR=/dev/shm']
WSPRD = ['wsprd', 'wsprsim_utils', 'wsprd_utils', 'tab', 'fano', 'jelinek', 'nhash']
ARMV7 = ['-target', 'arm-linux-musleabihf', '-mcpu=cortex_a8']
ARM64 = ['-target', 'aarch64-linux-musl']
CFLAGS = ['-O2', '-std=gnu11', '-Wall', '-ffunction-sections', '-fdata-sections', f'-ffile-prefix-map={HERE}=.']
LDFLAGS = ['-static', '-s', '-Wl,--gc-sections', '-Wl,--build-id=none']


def zig_cc(args, out, cache):
    env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(cache), ZIG_LOCAL_CACHE_DIR=str(cache))
    objs = []
    # wsprd's sources are built as WSJT-X ships them, so their warnings are
    # not ours to fix; agent.c and the two stand-ins are built strictly.
    for n in WSPRD:
        o = out.parent / f'{out.name}-{n}.o'
        subprocess.run(['zig', 'cc', *args, *CFLAGS, '-w', '-c', '-o', str(o), str(HERE / 'wsprd' / f'{n}.c')],
                       check=True, env=env)
        objs.append(str(o))
    subprocess.run(['zig', 'cc', *args, *CFLAGS, *LDFLAGS, '-Wextra', '-Werror', '-I', str(HERE), '-o', str(out),
                    str(HERE / 'agent.c'), str(HERE / 'wsprd' / 'fftw_compat.c'),
                    str(HERE / 'wsprd' / 'osdwspr.c'), str(HERE / 'grayline.c'), *objs, '-lm'],
                   check=True, env=env)
    for o in objs:
        os.unlink(o)


def check_version():
    agent = re.search(r'#define AGENT_VERSION "([^"]+)"', (HERE / 'agent.c').read_text())
    cfg = re.search(r'^Version: "([^"]+)"', (HERE / 'package' / f'{NAME}.cfg').read_text(), re.M)
    if not agent or not cfg or agent.group(1) != cfg.group(1):
        raise SystemExit(f'{NAME}.cfg Version and AGENT_VERSION in agent.c disagree')


def licenses():
    zig_lib = subprocess.run(['zig', 'env'], capture_output=True, text=True, check=True).stdout
    lib_dir = re.search(r'"?lib_dir"?\s*[:=]\s*"([^"]+)"', zig_lib).group(1)
    # UTF-8 whatever the locale: under cp1252 its 14 non-ASCII characters
    # would be re-encoded into a different LICENSES. (`python3 -I` ignores
    # PYTHONUTF8, so the locale can't be fixed from outside.)
    musl = pathlib.Path(lib_dir, 'libc', 'musl', 'COPYRIGHT').read_text(encoding='utf-8')
    return ('AetherWSPR is part of AetherSDR, licensed under the GNU GPL v3\n'
            '(https://github.com/aethersdr/AetherSDR). It includes the wsprd decoder\n'
            'from WSJT-X 2.7.0 (Joe Taylor K1JT, Steven Franke K9AN and others;\n'
            'GPL v3 or later; https://wsjt.sourceforge.io), whose source is in\n'
            'tools/wspr-agent/wsprd of the AetherSDR repository. The binaries are\n'
            'linked statically with musl libc, whose license follows.\n\n'
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
    check_version()
    a.output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='aether-wspr-') as t:
        stage = pathlib.Path(t)
        cache = stage / 'zig-cache'
        files = {
            f'{NAME}.cfg': ((HERE / 'package' / f'{NAME}.cfg').read_bytes(), 0o644),
            'launch.sh': ((HERE / 'package' / 'launch.sh').read_bytes(), 0o755),
        }
        zig_cc(ARMV7, stage / 'aether-wspr-armv7', cache)
        files['aether-wspr-armv7'] = ((stage / 'aether-wspr-armv7').read_bytes(), 0o755)
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

        zig_cc(ARM64, stage / 'aether-wspr', cache)
        version = re.search(r'#define AGENT_VERSION "([^"]+)"', (HERE / 'agent.c').read_text()).group(1)
        image = mkimage().build_image(NAME, [('aether-wspr', (stage / 'aether-wspr').read_bytes(), 0o755),
                                             ('LICENSES', license_text, 0o644)], version, CONTAINER_ENV)
        if len(image) > MAX_BYTES:
            raise SystemExit(f'image is {len(image)} bytes, over the {MAX_BYTES}-byte bound')
        image_path = a.output / f'{NAME}.tar.gz'
        image_path.write_bytes(image)
        print(f'built {image_path} ({len(image)} bytes)')
        if a.host:
            zig_cc(['-target', 'x86_64-linux-musl'], a.output / 'aether-wspr-host', cache)
            print(f'built {a.output / "aether-wspr-host"}')


if __name__ == '__main__':
    main()
