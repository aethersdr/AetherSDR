#!/usr/bin/env python3
"""Build an authenticated fixed-file release and its inspectable installation plan."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from cryptography.exceptions import InvalidSignature
import bundle
import units

NAMES = ('pd-boot', 'pd-update', 'payload.bundle', 'BOOT-LICENSES.txt',
         'aetherpd-boot.service', 'aetherpd-start.socket', 'aetherpd-start@.service',
         'aetherpd-shim.service')
PROFILES = {'flex8600': ('arm64', 1), 'flex6300': ('armv7', 2)}
HEADER = 80
MANIFEST = HEADER + 36 * len(NAMES)
PREFIX = MANIFEST + 64
MAX_BYTES = 18 * 1024 * 1024
LIMITS = (512*1024, 512*1024, bundle.MAX_PACKED, 512*1024, *(16*1024,)*4)


def assemble(files, profile, firmware, source_sha256, seed, paired_boot=True, managed_startup=False, origin_recovery=False, generation_selection=False, hold_guardians=True):
    if (profile not in PROFILES or set(files) != set(NAMES) or len(source_sha256) != 32
            or len(seed) != 32 or not re.fullmatch(r'[0-9]+(?:\.[0-9]+){3}', firmware)
            or len(firmware) >= 28):
        raise ValueError('invalid release profile, file set, firmware or signing input')
    if managed_startup and not paired_boot:
        raise ValueError('managed startup requires the paired boot contract')
    if origin_recovery and not managed_startup:
        raise ValueError('origin recovery requires managed startup')
    if generation_selection and not origin_recovery:
        raise ValueError('generation selection requires origin recovery')
    arch, profile_id = PROFILES[profile]
    expected_units = units.render(arch, managed_startup, hold_guardians)
    for name, expected in expected_units.items():
        if files[name] != expected.encode():
            raise ValueError('service definition differs from the fixed implementation: ' + name)
    for name in NAMES[:2]:
        bundle.elf(files[name], bundle.ARCH[arch])
    payload = files['payload.bundle']
    if payload[:8] != b'AEPDB001' or len(payload) < bundle.PREFIX or struct.unpack_from('>I', payload, 8)[0] != bundle.ARCH[arch]:
        raise ValueError('inner payload architecture or format does not match')
    key = Ed25519PrivateKey.from_private_bytes(seed)
    # The publisher must use the same configured trust key for both envelopes.
    key.public_key().verify(payload[bundle.PREFIX-64:bundle.PREFIX], payload[:bundle.PREFIX-64])
    records = []
    entries = []
    for name, maximum in zip(NAMES, LIMITS):
        raw = files[name]
        if not 0 < len(raw) <= maximum:
            raise ValueError('release member exceeds bound: ' + name)
        records.append(struct.pack('>I', len(raw)) + bundle.blake(raw))
        entries.append({'name': name, 'bytes': len(raw), 'sha256': hashlib.sha256(raw).hexdigest(),
                        'mode': '0755' if name in NAMES[:2] else '0644'})
    magic = b'AEPDR005' if generation_selection else b'AEPDR004' if origin_recovery else b'AEPDR003' if managed_startup else b'AEPDR002' if paired_boot else b'AEPDR001'
    manifest = (magic + struct.pack('>II', bundle.ARCH[arch], len(NAMES)) + source_sha256
                + struct.pack('>I', profile_id) + firmware.encode().ljust(28, b'\0') + b''.join(records))
    assert len(manifest) == MANIFEST
    release = manifest + key.sign(manifest) + b''.join(files[n] for n in NAMES)
    if len(release) > MAX_BYTES:
        raise ValueError('release exceeds maximum size')
    store = '/user/aetherpd' if arch == 'armv7' else '/var/lib/smartsdr/aetherpd'
    return release, {'format': magic.decode(), 'paired_boot': paired_boot, 'managed_startup': managed_startup, 'origin_recovery': origin_recovery, 'generation_selection': generation_selection, 'guardian_hold': hold_guardians, 'profile': profile, 'architecture': arch,
        'firmware': firmware, 'source_sha256': source_sha256.hex(), 'bytes': len(release),
        'sha256': hashlib.sha256(release).hexdigest(), 'blake2b_256': bundle.blake(release).hex(),
        'public_key': key.public_key().public_bytes_raw().hex(), 'files': entries,
        'persistent_store': store, 'runtime': '/run/aetherpd',
        'fixed_units': list(expected_units),
        'privileged_actions': ['verify approved release digest and trusted signature',
            'stage fixed authenticated files in private RAM',
            'retire the owned runtime before updating',
            'publish protected payload and boot files',
            'install fixed boot and local socket units',
            'start the ordinary-user primary shim after retiring any recognized predecessor'],
        'temporary_components': ['pd-update', 'installer/bootstrap staging and authority'],
        'rf_actions': [], 'automatic_calibration': False,
        'integration_status': 'release artifact only; this builder does not install, migrate, enable or start anything'}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--input', type=Path, required=True)
    p.add_argument('--profile', choices=PROFILES, required=True)
    p.add_argument('--firmware', required=True)
    p.add_argument('--source-sha256', required=True)
    p.add_argument('--signing-seed', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--legacy-boot', action='store_true', help='only for a preserved verifier predating the paired descriptor contract')
    p.add_argument('--managed-startup', action='store_true', help='require permanent boot helper primary/session entries')
    p.add_argument('--origin-recovery', action='store_true', help='attest boot helper support for version2 upgrade capsule cleanup')
    p.add_argument('--generation-selection', action='store_true', help='attest permanent boot support for approval-derived generations')
    a = p.parse_args()
    try:
        files = {}
        for name, maximum in zip(NAMES, LIMITS):
            path = a.input / name
            if path.is_symlink() or not path.is_file() or path.stat().st_size > maximum:
                raise ValueError('invalid release input: ' + name)
            files[name] = path.read_bytes()
        release, plan = assemble(files, a.profile, a.firmware, bytes.fromhex(a.source_sha256), a.signing_seed.read_bytes(), not a.legacy_boot, a.managed_startup, a.origin_recovery, a.generation_selection)
        a.output.parent.mkdir(parents=True, exist_ok=True)
        bundle.atomic_write(a.output, release)
        bundle.atomic_write(a.output.with_suffix('.plan.json'), (json.dumps(plan, indent=2)+'\n').encode())
    except (OSError, ValueError, InvalidSignature) as error:
        p.exit(1, str(error)+'\n')


if __name__ == '__main__':
    main()
