#!/usr/bin/env python3
"""Build AetherTailnet.ssdr_waveform: flex-tailnet-shim as a legacy (native)
waveform for FLEX-6000 radios.

The package is a ZIP with one AetherTailnet/ directory: the manifest, the
launcher (shim.sh), a static ARMv7 build of the shim reporting its version
as <shim version>-native, and the license bundle. Without --log-url it is
byte-for-byte reproducible with the pinned toolchain.

    python3 -I build.py --output OUT_DIR [--log-url http://<pc>:8765/] [--small]

--log-url bakes a diagnostic log sink into the launcher: the shim then POSTs
its recent log lines there every 15 s (see logsink.py). Use it only for a
test build; the address is in the package.
"""
import argparse
import gzip
import os
import pathlib
import re
import subprocess
import tempfile
import zipfile

HERE = pathlib.Path(__file__).resolve().parent
MODULE = HERE.parent  # tools/flex-tailnet-shim
NAME = 'AetherTailnet'
TOOLCHAIN = 'go1.27.1'
VARIANT = '-native'
# The radio's limit on a legacy upload is unknown: 5.5 MB has been proven on
# FLEX-6300 and 6700 radios and 8 MiB on a FLEX-8600. The package is about
# 9 MB, and that is also what it occupies once unpacked.
MAX_BYTES = 12 * 1024 * 1024
# --small leaves out Tailscale features the shim never uses, saving under
# 1 MB, for a radio that refuses the full package.
SMALL_TAGS = ('ts_omit_ssh,ts_omit_taildrop,ts_omit_drive,ts_omit_webclient,ts_omit_tap,'
              'ts_omit_kube,ts_omit_aws,ts_omit_capture,ts_omit_clientupdate,ts_omit_systray,'
              'ts_omit_relayserver,ts_omit_appconnectors,ts_omit_wakeonlan,ts_omit_logtail')
FIXED_TIME = (2026, 1, 1, 0, 0, 0)


def go(env, *args):
    return subprocess.run(['go', *args], cwd=MODULE, env=env, check=True,
                          capture_output=True, text=True).stdout


def build_env():
    return dict(os.environ, CGO_ENABLED='0', GOOS='linux', GOARCH='arm', GOARM='7',
                LC_ALL='C', GOFLAGS='-mod=vendor -buildvcs=false', GOTOOLCHAIN=TOOLCHAIN)


def licenses(env):
    """The GPL notice, then every linked module's license, as build.sh does."""
    parts = ['flex-tailnet-shim is part of AetherSDR and is licensed under the GNU GPL v3.\n'
             'It includes the following third-party Go modules, under their own licenses.\n']
    out = go(env, 'list', '-deps', '-f',
             '{{if .Module}}{{.Module.Path}} {{.Module.Version}}{{end}}', '.')
    for line in sorted({l for l in out.splitlines() if l.strip()}):
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


def launcher(log_url):
    text = (HERE / 'shim.sh').read_text()
    if log_url:
        if not re.fullmatch(r'http://[A-Za-z0-9.\-\[\]:]+(/[A-Za-z0-9._\-/]*)?', log_url):
            raise SystemExit('--log-url must be a plain http://host:port/path URL')
        text = text.replace('SHIM_LOG_URL=""', f'SHIM_LOG_URL="{log_url}"', 1)
    return text.encode()


def check_version():
    # The radio lists the waveform under the .cfg's Version; keep it the
    # version the shim itself reports, so the two never disagree.
    shim = re.search(r'^const shimVersion = "([^"]+)"', (MODULE / 'main.go').read_text(), re.M)
    cfg = re.search(r'^Version: "([^"]+)"', (HERE / f'{NAME}.cfg').read_text(), re.M)
    want = shim.group(1) + VARIANT if shim else None
    if not cfg or cfg.group(1) != want:
        raise SystemExit(f'{NAME}.cfg Version is {cfg.group(1) if cfg else "missing"}, '
                         f'expected {want} (shimVersion + {VARIANT})')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--output', required=True, type=pathlib.Path)
    ap.add_argument('--log-url', default='', help='diagnostic log sink baked into the launcher')
    ap.add_argument('--small', action='store_true', help='omit unused Tailscale features (about 0.8 MB smaller)')
    a = ap.parse_args()
    check_version()
    a.output.mkdir(parents=True, exist_ok=True)
    env = build_env()
    with tempfile.TemporaryDirectory(prefix='aether-tailnet-') as t:
        stage = pathlib.Path(t)
        variant = VARIANT + ('-small' if a.small else '')
        tags = ['-tags', SMALL_TAGS] if a.small else []
        go(env, 'build', *tags, '-trimpath', f'-ldflags=-s -w -X main.buildVariant={variant}',
           '-o', str(stage / 'flex-tailnet-shim-armv7'), '.')
        go(env, 'build', '-trimpath', '-ldflags=-s -w', '-o', str(stage / 'unpack-armv7'), './native/unpack')
        # The shim ships gzipped and is unpacked into RAM at each start: a
        # FLEX-6700's /user has about 13 MB free and the shim is 20 MB.
        # mtime=0 and no file name keep the bytes reproducible.
        shim_gz = gzip.compress((stage / 'flex-tailnet-shim-armv7').read_bytes(), compresslevel=9, mtime=0)
        files = {
            f'{NAME}.cfg': ((HERE / f'{NAME}.cfg').read_bytes(), 0o644, zipfile.ZIP_DEFLATED),
            'shim.sh': (launcher(a.log_url), 0o755, zipfile.ZIP_DEFLATED),
            'unpack-armv7': ((stage / 'unpack-armv7').read_bytes(), 0o755, zipfile.ZIP_DEFLATED),
            'flex-tailnet-shim-armv7.gz': (shim_gz, 0o644, zipfile.ZIP_STORED),
            'LICENSES': (licenses(env).encode(), 0o644, zipfile.ZIP_DEFLATED),
        }
        target = a.output / f'{NAME}.ssdr_waveform'
        tmp = target.with_suffix('.tmp')
        with zipfile.ZipFile(tmp, 'w', zipfile.ZIP_DEFLATED) as z:
            for name, (data, mode, method) in files.items():
                info = zipfile.ZipInfo(f'{NAME}/{name}', FIXED_TIME)
                info.create_system = 3
                info.compress_type = method
                info.external_attr = (0o100000 | mode) << 16
                z.writestr(info, data, compresslevel=9 if method == zipfile.ZIP_DEFLATED else None)
        size = tmp.stat().st_size
        if size > MAX_BYTES:
            tmp.unlink()
            raise SystemExit(f'package is {size} bytes, over the {MAX_BYTES}-byte bound')
        os.replace(tmp, target)
    extra = (' small' if a.small else '') + (' with log sink ' + a.log_url if a.log_url else '')
    print(f'built {target} ({size} bytes){extra}')


if __name__ == '__main__':
    main()
