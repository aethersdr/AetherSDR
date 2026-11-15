"""Assemble a public first-install request after signature checks and consent.

This encoder grants no authorization. The uploader must authenticate the release
key independently and require approval of the exact plan digest before upload.
The admin bearer token and transport key must never be supplied to this module.
"""
import io
from pathlib import Path
import sys
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'boot'))
import bundle
import delivery_consent


def build(requester, worker, plan, release, licenses):
    bundle.elf(requester, 64)
    if len(plan) != 308 or plan[:8] != b'PDFST001':
        raise ValueError('explicit first-install plan required')
    receipt = delivery_consent.encode(worker, plan)
    if bundle.blake(release) != plan[24:56]:
        raise ValueError('release does not match approved plan')
    public = b'PDFPUB01' + receipt + plan[244:276]
    name = 'AetherPDFirst-' + plan[244:252].hex()
    cfg = f'''[header]
Name: {name}
Version: "0.1.0"
Minimum-SmartSDR-Version: 4.2.18.0
Author: AetherSDR
License: GPL-3.0
Executable: "start.sh"

[setup]

[end]
'''.encode()
    files = {name + '.cfg': (cfg, 0o644),
             'start.sh': (b'#!/bin/sh\nset -eu\nexec "${0%/*}/pd-first-request"\n', 0o755),
             'pd-first-request': (requester, 0o755), 'request': (public, 0o444),
             'worker': (worker, 0o444), 'plan': (plan, 0o444),
             'release': (release, 0o444), 'LICENSES': (licenses, 0o444)}
    output = io.BytesIO()
    with zipfile.ZipFile(output, 'w', zipfile.ZIP_DEFLATED) as archive:
        for leaf, (data, mode) in sorted(files.items()):
            entry = zipfile.ZipInfo(name + '/' + leaf, (2026, 1, 1, 0, 0, 0))
            entry.create_system = 3
            entry.external_attr = (0o100000 | mode) << 16
            entry.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(entry, data)
    return name, output.getvalue(), public
