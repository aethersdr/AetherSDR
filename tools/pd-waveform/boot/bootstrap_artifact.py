#!/usr/bin/env python3
"""Sign a bounded privileged bootstrap after native verification of its release.

This publisher tool installs nothing and grants no operator consent. The shipping
delivery endpoint must pin the publisher key independently of this artifact.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import stat
import struct
import subprocess
import tempfile
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
import bundle
import release

HEADER = 120
PREFIX = 184
MAX_WORKER = 512 * 1024
ROLES = {'install': 1, 'upgrade': 2}


def assemble(worker, arch, role, source, release_digest, seed):
    if (arch not in (7, 64) or role not in ROLES.values()
            or role == ROLES['install'] and arch != 64
            or len(source) != 32 or len(release_digest) != 32 or len(seed) != 32
            or not 0 < len(worker) <= MAX_WORKER):
        raise ValueError('invalid bootstrap role, architecture, size or digest')
    bundle.elf(worker, arch)
    header = (b'AEPDX001' + struct.pack('>IIII', arch, role, len(worker), 0)
              + source + release_digest + bundle.blake(worker))
    assert len(header) == HEADER
    key = Ed25519PrivateKey.from_private_bytes(seed)
    return header + key.sign(header) + worker


def bounded_file(path, minimum, maximum, private=False):
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    try:
        info = os.fstat(fd)
        if (not stat.S_ISREG(info.st_mode) or not minimum <= info.st_size <= maximum
                or private and (info.st_mode & 0o077 or info.st_nlink != 1)):
            raise ValueError('invalid bounded input file')
        with os.fdopen(fd, 'rb') as stream:
            fd = -1
            data = stream.read(maximum + 1)
    finally:
        if fd >= 0:
            os.close(fd)
    if len(data) != info.st_size:
        raise ValueError('input changed during read')
    return data


def publish(worker_path, release_path, seed_path, role, output, verifier):
    worker = bounded_file(worker_path, 52, MAX_WORKER)
    data = bounded_file(release_path, release.PREFIX, release.MAX_BYTES)
    seed = bounded_file(seed_path, 32, 32, private=True)
    key = Ed25519PrivateKey.from_private_bytes(seed).public_key().public_bytes_raw()
    arch = struct.unpack_from('>I', data, 8)[0]
    profile = struct.unpack_from('>I', data, 48)[0]
    if (arch, profile) not in ((64, 1), (7, 2)):
        raise ValueError('invalid release target')
    firmware = data[52:80].split(b'\0', 1)[0].decode('ascii')
    digest = bundle.blake(data)
    # Verify the same private snapshot whose digest is bound in the artifact.
    with tempfile.TemporaryDirectory(prefix='pd-bootstrap-verify-') as directory:
        root = Path(directory)
        for name, raw in [('release', data), ('key', key), ('digest', digest)]:
            (root/name).write_bytes(raw)
        subprocess.run([str(verifier), str(root/'release'), str(root/'key'), str(arch),
                        str(profile), firmware, str(root/'digest')], check=True,
                       capture_output=True, timeout=30)
    artifact = assemble(worker, arch, ROLES[role], data[16:48], digest, seed)
    bundle.atomic_write(output, artifact)
    return {'format': 'AEPDX001', 'role': role, 'arch': arch, 'bytes': len(artifact),
            'sha256': hashlib.sha256(artifact).hexdigest(),
            'worker_sha256': hashlib.sha256(worker).hexdigest(),
            'release_blake2b_256': digest.hex(), 'source_sha256': data[16:48].hex(),
            'public_key': key.hex(), 'grants_consent': False, 'executes': False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('worker', 'release', 'signing-seed', 'output', 'verifier'):
        parser.add_argument('--'+name, type=Path, required=True)
    parser.add_argument('--role', choices=ROLES, required=True)
    args = parser.parse_args()
    try:
        result = publish(args.worker, args.release, args.signing_seed, args.role,
                         args.output, args.verifier.resolve())
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        parser.exit(1, str(error)+'\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
