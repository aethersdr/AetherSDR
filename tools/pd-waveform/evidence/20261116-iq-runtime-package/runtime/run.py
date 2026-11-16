"""Offline two-path runtime rebuild; the development signing seed stays in pipes/RAM."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import time
import zlib
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

WORK = Path('/home/jeremy/AetherSDR/build/aetherpd-source')
OUT = Path(__file__).resolve().parent
GO = Path('/home/jeremy/go/pkg/mod/golang.org/toolchain@v0.0.1-go1.27.1.linux-amd64/bin/go')


def sha(data):
    return hashlib.sha256(data).hexdigest()


def child(root, output, arch):
    settings = json.load(sys.stdin)
    seed = bytes.fromhex(settings['seed'])
    source = bytes.fromhex(settings['source'])
    public = Ed25519PrivateKey.from_private_bytes(seed).public_key().public_bytes_raw()
    output.mkdir(parents=True, exist_ok=False)
    sys.path.insert(0, str(root / 'tools/pd-waveform/boot'))
    import build as bootbuild
    import bundle
    import release
    import units
    spec = importlib.util.spec_from_file_location('dsp_build', root / 'tools/pd-waveform/build.py')
    dsp = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(dsp)
    env = dict(os.environ, CGO_ENABLED='0', GOOS='linux', GOARCH='arm' if arch == 'armv7' else 'arm64',
               GOTOOLCHAIN='local', GOPROXY='off', GOCACHE=str(output / 'go-cache'))
    env.pop('AETHER_PD_SOURCE_ROOT', None)
    if arch == 'armv7':
        env['GOARM'] = '7'
    else:
        env.pop('GOARM', None)
    print(f'Building {arch} shim from {root}', flush=True)
    subprocess.run([str(GO), 'build', '-mod=vendor', '-trimpath', '-buildvcs=false', '-ldflags=-s -w',
                    '-o', str(output / 'flex-tailnet-shim'), '.'],
                   cwd=root / 'tools/flex-tailnet-shim', env=env, check=True)
    print(f'Building {arch} session helpers and DSP', flush=True)
    subprocess.run([sys.executable, str(root / 'tools/pd-waveform/session/build.py'),
                    '--arch', arch, '--output', str(output / 'session')], env=env, check=True)
    target = dsp.ARMV7 if arch == 'armv7' else dsp.ARM64
    dsp.zig_cc(target, output / 'aether-pd', output / 'dsp-cache')
    dsp.zig_unkey_guardian(target, output / 'pd-unkey-guard', output / 'dsp-cache')
    files = {}
    for name in bundle.NAMES:
        path = (root / 'runtime-LICENSES' if name == 'LICENSES' else
                output / name if name in ('flex-tailnet-shim', 'aether-pd', 'pd-unkey-guard') else
                output / 'session' / (name + '-' + arch))
        files[name] = path.read_bytes()
    payload, payload_meta = bundle.assemble(files, bundle.ARCH[arch], source, seed)
    (output / 'payload.bundle').write_bytes(payload)
    for entry in ('pd_boot.c', 'pd_update.c'):
        bootbuild.build(output / 'boot', arch, entry, public)
    release_files = {'pd-boot': (output / 'boot/pd-boot').read_bytes(),
                     'pd-update': (output / 'boot/pd-update').read_bytes(), 'payload.bundle': payload,
                     'BOOT-LICENSES.txt': (root / 'LICENSE').read_bytes() + b'\n' +
                         (root / 'tools/pd-waveform/boot/BOOT-LICENSES.txt').read_bytes(),
                     **{name: text.encode() for name, text in units.render(arch, True).items()}}
    profile, firmware = ('flex6300', '4.2.18.41174') if arch == 'armv7' else ('flex8600', '4.3.16.42034')
    artifact, release_meta = release.assemble(release_files, profile, firmware, source, seed,
        paired_boot=True, managed_startup=True, origin_recovery=True, generation_selection=True)
    (output / 'candidate.release').write_bytes(artifact)
    (output / 'public-key').write_bytes(public)
    (output / 'release-digest').write_bytes(bundle.blake(artifact))
    checker = bootbuild.build(output / 'verify', 'host', 'release_check.c')
    subprocess.run([str(checker), str(output / 'candidate.release'), str(output / 'public-key'),
                    str(bundle.ARCH[arch]), str(release.PROFILES[profile][1]), firmware,
                    str(output / 'release-digest')], check=True)
    metadata = {'payload': payload_meta, 'release': release_meta, 'native_release_verified': True,
                'session_helpers': {p.name: sha(p.read_bytes()) for p in sorted((output / 'session').glob('*-' + arch))}}
    (output / 'manifest.json').write_text(json.dumps(metadata, indent=2) + '\n')
    print(f'Finished {arch}: release={len(artifact)} expanded={payload_meta["expanded_bytes"]}', flush=True)


def main():
    assert not subprocess.check_output(['git', 'status', '--porcelain'], cwd=WORK).strip()
    prefixes = ('tools/pd-waveform/', 'tools/flex-tailnet-shim/', 'third_party/wdsp/',
                'third_party/fftw3/', 'third_party/monocypher/', 'third_party/zlib/')
    tracked = subprocess.check_output(['git', 'ls-files', '-z'], cwd=WORK).decode().split('\0')
    paths = sorted(n for n in tracked if n == 'LICENSE' or n.startswith(prefixes))
    hashes = {name: sha((WORK / name).read_bytes()) for name in paths}
    prior = OUT.parent / 'post-rollback-upgrade-development/candidate'
    licenses = (prior / 'arm64/payload/LICENSES').read_bytes()
    expected = json.loads((prior / 'build-report.json').read_text())['architectures']['arm64']['payload_files']['LICENSES']
    assert sha(licenses) == expected
    hashes['runtime-LICENSES'] = sha(licenses)
    source_manifest = (json.dumps(hashes, sort_keys=True, separators=(',', ':')) + '\n').encode()
    (OUT / 'source-sha256.json').write_bytes(source_manifest)
    key = Ed25519PrivateKey.generate()
    settings = json.dumps({'seed': key.private_bytes_raw().hex(), 'source': sha(source_manifest)})
    start = time.monotonic()
    for label in ('short', 'second-distinct-long-source-path'):
        root = OUT / label / 'source'
        root.mkdir(parents=True, exist_ok=False)
        for name in paths:
            dest = root / name
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(WORK / name, dest)
        (root / 'runtime-LICENSES').write_bytes(licenses)
        for arch in ('armv7', 'arm64'):
            output = OUT / label / arch
            with (OUT / (label + '-' + arch + '.log')).open('w') as log:
                subprocess.run([sys.executable, str(Path(__file__).resolve()), '--child', str(root), str(output), arch],
                               input=settings, text=True, stdout=log, stderr=subprocess.STDOUT, check=True)
            print(f'Completed {label}/{arch}', flush=True)
    architectures = {}
    for arch in ('armv7', 'arm64'):
        a, b = (OUT / label / arch for label in ('short', 'second-distinct-long-source-path'))
        first, second = (json.loads((p / 'manifest.json').read_text()) for p in (a, b))
        architectures[arch] = {'identical_manifest': first == second,
            'identical_release': (a / 'candidate.release').read_bytes() == (b / 'candidate.release').read_bytes(),
            'first': first, 'second': second}
    report = {'source_commit': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=WORK, text=True).strip(),
        'source_manifest_sha256': sha(source_manifest), 'script_sha256': sha(Path(__file__).read_bytes()),
        'source_files': len(hashes), 'python': platform.python_version(), 'zlib': zlib.ZLIB_RUNTIME_VERSION,
        'zig': subprocess.check_output(['zig', 'version'], text=True).strip(),
        'go': subprocess.check_output([str(GO), 'version'], text=True).strip(),
        'go_binary_sha256': sha(GO.read_bytes()), 'zig_binary_sha256': sha(Path(shutil.which('zig')).read_bytes()),
        'separate_source_output_and_compiler_caches': True, 'same_host_and_toolchains': True,
        'development_signing_seed_persisted': False, 'installed': False, 'production_ready': False,
        'elapsed_seconds': time.monotonic() - start, 'architectures': architectures}
    (OUT / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    assert all(a['identical_manifest'] and a['identical_release'] for a in architectures.values())
    print('Both complete runtime releases reproduced byte for byte.', flush=True)


if __name__ == '__main__':
    if len(sys.argv) > 1 and sys.argv[1] == '--child':
        child(Path(sys.argv[2]), Path(sys.argv[3]), sys.argv[4])
    else:
        main()
