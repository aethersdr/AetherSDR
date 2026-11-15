#!/usr/bin/env python3
"""Build a signed, deterministic fixed-layout AetherPD payload; never install it."""
import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import struct
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

NAMES = ('flex-tailnet-shim', 'aether-pd', 'pd-primary-launch', 'pd-start-session',
         'pd-radio-launch', 'pd-fixed-arp', 'pd-iq-source', 'pd-capture-bootstrap',
         'pd-unkey-guard', 'pd-lease-guard', 'LICENSES')
ARCH = {'armv7': 7, 'arm64': 64}
MAX_PACKED = 16 * 1024 * 1024
MAX_EXPANDED = 48 * 1024 * 1024
PREFIX = 48 + 72 * len(NAMES) + 64


def blake(data):
    return hashlib.blake2b(data, digest_size=32).digest()


def elf(data, arch):
    minimum, kind, machine = (52, 1, 40) if arch == 7 else (64, 2, 183)
    if (len(data) < minimum or data[:4] != b'\x7fELF' or data[4:7] != bytes([kind, 1, 1])
            or struct.unpack_from('<HH', data, 16) != (2, machine)):
        raise ValueError('payload must be a matching little-endian ELF executable')


def assemble(files, arch, source_sha256, seed):
    if arch not in ARCH.values() or len(source_sha256) != 32 or len(seed) != 32 or set(files) != set(NAMES):
        raise ValueError('wrong architecture, source hash, signing seed, or fixed file set')
    records = []
    compressed = []
    manifest_files = []
    expanded_total = 0
    for name in NAMES:
        raw = files[name]
        limit = 4 * 1024 * 1024 if name == 'LICENSES' else 32 * 1024 * 1024
        if not 1 <= len(raw) <= limit:
            raise ValueError('invalid expanded size: ' + name)
        if name != 'LICENSES':
            elf(raw, arch)
        expanded_total += len(raw)
        packed = gzip.compress(raw, compresslevel=9, mtime=0)
        records.append(struct.pack('>II', len(packed), len(raw)) + blake(packed) + blake(raw))
        compressed.append(packed)
        manifest_files.append({'name': name, 'bytes': len(raw), 'gzip_bytes': len(packed),
                               'sha256': hashlib.sha256(raw).hexdigest()})
    if expanded_total > MAX_EXPANDED or PREFIX + sum(map(len, compressed)) > MAX_PACKED:
        raise ValueError('payload exceeds bounded boot format')
    manifest = b'AEPDB001' + struct.pack('>II', arch, len(NAMES)) + source_sha256 + b''.join(records)
    key = Ed25519PrivateKey.from_private_bytes(seed)
    package = manifest + key.sign(manifest) + b''.join(compressed)
    return package, {'format': 'AEPDB001', 'arch': arch, 'files': manifest_files,
                     'source_sha256': source_sha256.hex(), 'expanded_bytes': expanded_total,
                     'bytes': len(package), 'sha256': hashlib.sha256(package).hexdigest(),
                     'blake2b_256': blake(package).hex(), 'public_key': key.public_key().public_bytes_raw().hex()}


def atomic_write(path, data):
    temporary = path.with_name(path.name + '.new')
    with temporary.open('xb') as output:
        output.write(data)
        output.flush()
        os.fsync(output.fileno())
    os.replace(temporary, path)
    parent = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(parent)
    finally:
        os.close(parent)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', type=Path, required=True)
    parser.add_argument('--arch', choices=ARCH, required=True)
    parser.add_argument('--source-sha256', required=True)
    parser.add_argument('--signing-seed', type=Path, required=True, help='private raw 32-byte Ed25519 seed')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    try:
        files = {}
        for name in NAMES:
            path = args.input / name
            if path.is_symlink() or not path.is_file() or path.stat().st_size > 32 * 1024 * 1024:
                raise ValueError('invalid input file: ' + name)
            files[name] = path.read_bytes()
        package, report = assemble(files, ARCH[args.arch], bytes.fromhex(args.source_sha256), args.signing_seed.read_bytes())
        args.output.parent.mkdir(parents=True, exist_ok=True)
        atomic_write(args.output, package)
        atomic_write(args.output.with_suffix('.json'), (json.dumps(report, indent=2) + '\n').encode())
    except (OSError, ValueError) as error:
        parser.exit(1, str(error) + '\n')


if __name__ == '__main__':
    main()
