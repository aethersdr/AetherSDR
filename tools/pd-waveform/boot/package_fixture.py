#!/usr/bin/env python3
"""Generate public test vectors for the desktop loader using native publishers.

These deterministic signing seeds are test data, never release trust anchors.
The JSON combines inputs and expected trust only for offline tests. Production
trust must come independently from the authenticated application catalogue.
"""
import hashlib
import json
from pathlib import Path
import struct
import bootstrap_artifact
import bundle
import release
import units
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey


def fixture(arch):
    platform, profile = ('arm64', 'flex8600') if arch == 64 else ('armv7', 'flex6300')
    executable = bytearray(64)
    executable[:7] = bytes.fromhex('7f454c46') + bytes([2 if arch == 64 else 1, 1, 1])
    struct.pack_into('<HH', executable, 16, 2, 183 if arch == 64 else 40)
    executable = bytes(executable)
    releases, keys, seeds, sources = [], [], [], []
    for label in ('candidate', 'previous'):
        seed = hashlib.sha256(('public desktop package fixture ' + label).encode()).digest()
        source = hashlib.sha256(label.encode()).digest()
        payload, _ = bundle.assemble({name: b'fixture license' if name == 'LICENSES' else executable
                                     for name in bundle.NAMES}, arch, source, seed)
        files = {'pd-boot': executable, 'pd-update': executable, 'payload.bundle': payload,
                 'BOOT-LICENSES.txt': b'fixture license',
                 **{name: text.encode() for name, text in units.render(platform, True).items()}}
        data, _ = release.assemble(files, profile, '4.3.16.42034', source, seed,
                                  managed_startup=True, origin_recovery=True, generation_selection=True)
        releases.append(data)
        keys.append(Ed25519PrivateKey.from_private_bytes(seed).public_key().public_bytes_raw())
        seeds.append(seed)
        sources.append(source)
    worker = bootstrap_artifact.assemble(executable, arch, 2, sources[0], bundle.blake(releases[0]), seeds[0])
    files = dict(zip(('worker.envelope', 'candidate.release', 'previous.release', 'pd-accept',
                      'pd-carrier', 'pd-send', 'pd-deliver', 'LICENSES'),
                     (worker, *releases, executable, executable, executable, executable, b'fixture license')))
    result = {'test_only': True, 'arch': arch, 'candidate_key': keys[0].hex(), 'previous_key': keys[1].hex(),
            'files': {name: data.hex() for name, data in files.items()},
            'pins': {name: {'size': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
                     for name, data in files.items()},
            'wrong_source_worker': bootstrap_artifact.assemble(executable, arch, 2, sources[1], bundle.blake(releases[0]), seeds[0]).hex(),
            'wrong_release_worker': bootstrap_artifact.assemble(executable, arch, 2, sources[0], bundle.blake(releases[1]), seeds[0]).hex()}
    if arch == 64:
        import sys
        sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'request'))
        import first_install_package
        import first_install
        import approval
        first_worker = bootstrap_artifact.assemble(executable, arch, 1, sources[0], bundle.blake(releases[0]), seeds[0])
        plan = first_install.encode(approval.encode(1, '4.3.16.42034', 'fixture-8600', bundle.blake(releases[0])),
                                    b't'*32, 'fixture-radio', b'n'*32)
        _, native, _ = first_install_package.build(executable, first_worker, plan, releases[0], b'fixture license')
        result.update(first_worker=first_worker.hex(), first_plan=plan.hex(), first_native_sha256=hashlib.sha256(native).hexdigest())
    return result


if __name__ == '__main__':
    directory = Path(__file__).parent / 'testdata'
    directory.mkdir(exist_ok=True)
    for arch in (64, 7):
        (directory / f'package-{arch}.json').write_text(json.dumps(fixture(arch), indent=2) + '\n')
