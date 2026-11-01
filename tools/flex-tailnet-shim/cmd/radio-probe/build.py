#!/usr/bin/env python3
"""Build AetherRadioProbe.ssdr_waveform, the legacy (native) waveform package.

The package is a ZIP holding one AetherRadioProbe/ directory: the manifest,
a launcher that picks the binary by `uname -m`, static ARM64 and ARMv7 builds
and the license bundle. It is byte-for-byte reproducible: fixed timestamps,
modes and file order, and binaries built with -trimpath -buildvcs=false by
the shim's pinned toolchain, so the probe tests exactly what the shim would
be built with.

    python3 -I build.py --output OUT_DIR
"""
import argparse
import os
import pathlib
import subprocess
import tempfile
import zipfile

HERE = pathlib.Path(__file__).resolve().parent
MODULE = HERE.parents[1]  # tools/flex-tailnet-shim
NAME = 'AetherRadioProbe'
TOOLCHAIN = 'go1.27.1'
# The legacy upload is bounded by what has been proven on a radio; the
# 8600 accepted 8 MiB.
MAX_BYTES = 8 * 1024 * 1024
FIXED_TIME = (2026, 1, 1, 0, 0, 0)


def go(env, *args):
    return subprocess.run(['go', *args], cwd=MODULE, env=env, check=True,
                          capture_output=True, text=True).stdout


def build_binaries(stage):
    env = dict(os.environ, CGO_ENABLED='0', GOOS='linux', LC_ALL='C',
               GOFLAGS='-mod=vendor -buildvcs=false', GOTOOLCHAIN=TOOLCHAIN)
    targets = {'radio-probe-arm64': {'GOARCH': 'arm64'},
               'radio-probe-armv7': {'GOARCH': 'arm', 'GOARM': '7'}}
    for out, extra in targets.items():
        go(dict(env, **extra), 'build', '-trimpath', '-ldflags=-s -w',
           '-o', str(stage / out), './cmd/radio-probe')
    return env


def licenses(env):
    """The GPL notice, then every linked module's license, as build.sh does."""
    parts = ['AetherRadioProbe is part of AetherSDR and is licensed under the GNU GPL v3.\n'
             'It includes the following third-party Go modules, under their own licenses.\n']
    mods = set()
    for arch in ({'GOARCH': 'arm64'}, {'GOARCH': 'arm', 'GOARM': '7'}):
        out = go(dict(env, **arch), 'list', '-deps', '-f',
                 '{{if .Module}}{{.Module.Path}} {{.Module.Version}}{{end}}', './cmd/radio-probe')
        mods.update(l for l in out.splitlines() if l.strip())
    for line in sorted(mods):
        path, _, version = line.partition(' ')
        if path == 'github.com/aethersdr/AetherSDR/tools/flex-tailnet-shim':
            continue
        d = MODULE / 'vendor' / path
        lic = sorted(p for p in os.listdir(d) if p.lower().startswith(('licen', 'copying')))
        parts.append('\n' + '=' * 64 + f'\n{path} {version}\n' + '=' * 64 + '\n')
        parts.append((d / lic[0]).read_text())
        for n in sorted(p for p in os.listdir(d) if p.lower().startswith('notice')):
            parts.append('\n' + (d / n).read_text())
    goroot = go(env, 'env', 'GOROOT').strip()
    gover = go(env, 'env', 'GOVERSION').strip()
    parts.append('\n' + '=' * 64 + f'\nGo standard library {gover}\n' + '=' * 64 + '\n')
    parts.append(pathlib.Path(goroot, 'LICENSE').read_text())
    return ''.join(parts)


def package(stage, target):
    files = [(f'{NAME}.cfg', 0o644), ('probe.sh', 0o755), ('radio-probe-arm64', 0o755),
             ('radio-probe-armv7', 0o755), ('LICENSES', 0o644)]
    tmp = target.with_suffix('.tmp')
    with zipfile.ZipFile(tmp, 'w', zipfile.ZIP_DEFLATED) as z:
        for name, mode in files:
            info = zipfile.ZipInfo(f'{NAME}/{name}', FIXED_TIME)
            info.create_system = 3
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = (0o100000 | mode) << 16
            z.writestr(info, (stage / name).read_bytes(), compresslevel=9)
    size = tmp.stat().st_size
    if size > MAX_BYTES:
        tmp.unlink()
        raise SystemExit(f'package is {size} bytes, over the {MAX_BYTES}-byte bound')
    os.replace(tmp, target)
    return size


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--output', required=True, type=pathlib.Path, help='directory for the package')
    args = ap.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='radio-probe-') as t:
        stage = pathlib.Path(t)
        for name in (f'{NAME}.cfg', 'probe.sh'):
            (stage / name).write_bytes((HERE / 'package' / name).read_bytes())
        env = build_binaries(stage)
        (stage / 'LICENSES').write_text(licenses(env))
        target = args.output / f'{NAME}.ssdr_waveform'
        size = package(stage, target)
    print(f'built {target} ({size} bytes)')


if __name__ == '__main__':
    main()
