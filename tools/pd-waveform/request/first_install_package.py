"""Assemble a public first-install request after signature checks and consent.

This encoder grants no authorization. The uploader must authenticate the release
key independently and require approval of the exact plan digest before upload.
The admin bearer token and transport key must never be supplied to this module.
"""
import io
import gzip
import hashlib
import importlib.util
import json
from pathlib import Path
import sys
import zipfile
import tarfile

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
    name = 'AetherPDFirst' + plan[244:252].hex()
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


def verify_uploads(native, carrier, plan):
    """Bind both canonical public archives to the exact operator-approved plan.

    Publisher executable authentication remains the distributor's responsibility;
    this check prevents mixing independently approved plans and upload records.
    """
    with zipfile.ZipFile(io.BytesIO(native)) as archive:
        entries = archive.infolist()
        if len(entries) != 8 or sum(x.file_size for x in entries) > 24*1024*1024:
            raise ValueError('native archive exceeds fixed shape or size')
        name = 'AetherPDFirst' + plan[244:252].hex()
        content = lambda leaf: archive.read(name + '/' + leaf)
        if content('plan') != plan:
            raise ValueError('native archive differs from approved plan')
        expected_name, canonical, public = build(content('pd-first-request'), content('worker'),
                                                plan, content('release'), content('LICENSES'))
        if canonical != native or expected_name != name:
            raise ValueError('noncanonical native first-install archive')

    def unpack(data):
        with gzip.GzipFile(fileobj=io.BytesIO(data)) as compressed:
            expanded = compressed.read(32*1024*1024+1)
        if len(expanded) > 32*1024*1024:
            raise ValueError('carrier expansion exceeds bound')
        return tarfile.open(fileobj=io.BytesIO(expanded))

    with unpack(carrier) as outer:
        def read_json(path):
            member = outer.getmember(path)
            if not member.isfile() or member.size > 16384:
                raise ValueError('carrier metadata exceeds bound')
            return json.load(outer.extractfile(member))
        index = read_json('index.json')
        digest = index['manifests'][0]['digest']
        manifest = read_json('blobs/sha256/' + digest.removeprefix('sha256:'))
        layer = manifest['layers'][0]
        member = outer.getmember('blobs/sha256/' + layer['digest'].removeprefix('sha256:'))
        if not member.isfile() or member.size > 8*1024*1024:
            raise ValueError('carrier layer exceeds bound')
        data = outer.extractfile(member).read()
        if 'sha256:' + hashlib.sha256(data).hexdigest() != layer['digest']:
            raise ValueError('carrier layer digest mismatch')
    with unpack(data) as layer:
        members = layer.getmembers()
        if len(members) != 7 or sum(x.size for x in members) > 24*1024*1024:
            raise ValueError('unexpected carrier layer shape')
        if layer.extractfile('pd/request').read() != public:
            raise ValueError('carrier differs from approved first-install record')
        executables = {leaf: layer.extractfile('pd/'+leaf).read() for leaf in ['accept','carrier','send','deliver']}
        licenses = layer.extractfile('LICENSES').read()
    spec = importlib.util.spec_from_file_location('first_carrier_image', Path(__file__).resolve().parents[1]/'carrier/image.py')
    image = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(image)
    canonical, info = image.build(executables, public, licenses)
    if canonical != carrier:
        raise ValueError('noncanonical first-install carrier')
    return name, info['name']
