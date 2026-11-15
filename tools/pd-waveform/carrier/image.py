"""Build a public-record, per-request ARM64 carrier image. This does not grant consent.

The caller must authenticate the distributed executables and obtain approval of
the exact plan before supplying its public request record. No shell is included.
"""
import hashlib
import importlib.util
import json
from pathlib import Path
import struct

source = Path(__file__).resolve().parents[2] / 'flex-tailnet-shim' / 'mkimage.py'
spec = importlib.util.spec_from_file_location('shim_image', source)
helpers = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helpers)


def build(executables, configuration, licenses):
    if set(executables) != {'accept', 'carrier', 'send', 'deliver'}:
        raise ValueError('fixed carrier executable set required')
    if (len(configuration) != 120 or configuration[:8] != b'PDPUB001'
            or not any(configuration[88:]) or configuration[8:16] != b'PDDELIV1'
            or struct.unpack_from('>II', configuration, 16) != (64, 2)):
        raise ValueError('invalid public ARM64 upgrade request record')
    files = [('pd', None, 0o700)]
    for name in sorted(executables):
        data = executables[name]
        if (len(data) < 64 or data[:6] != b'\x7fELF\x02\x01'
                or struct.unpack_from('<H', data, 18)[0] != 183):
            raise ValueError('expected ARM64 ELF executable')
        files.append(('pd/' + name, data, 0o500))
    files += [('pd/request', configuration, 0o444), ('LICENSES', licenses, 0o444)]
    layer_tar = helpers.tar_bytes(files)
    layer = helpers.gz(layer_tar)
    config = {
        'architecture': 'arm64', 'os': 'linux',
        'config': {'Entrypoint': ['/pd/accept'], 'User': '0:0',
                   'Labels': {'aetherpd.carrier': 'v1',
                              'com.flexradio.waveform.name': 'aetherpd-carrier',
                              'com.flexradio.waveform.version': '0.1.0'}},
        'rootfs': {'type': 'layers', 'diff_ids': [helpers.sha(layer_tar)]},
        'history': [{'created_by': 'tools/pd-waveform/carrier/image.py'}],
    }
    encode = lambda value: json.dumps(value, sort_keys=True, separators=(',', ':')).encode()
    config_bytes = encode(config)
    manifest = {'schemaVersion': 2, 'mediaType': 'application/vnd.oci.image.manifest.v1+json',
                'config': {'mediaType': 'application/vnd.oci.image.config.v1+json',
                           'digest': helpers.sha(config_bytes), 'size': len(config_bytes)},
                'layers': [{'mediaType': 'application/vnd.oci.image.layer.v1.tar+gzip',
                            'digest': helpers.sha(layer), 'size': len(layer)}]}
    manifest_bytes = encode(manifest)
    index = {'schemaVersion': 2, 'manifests': [{'mediaType': manifest['mediaType'],
             'digest': helpers.sha(manifest_bytes), 'size': len(manifest_bytes),
             'annotations': {'org.opencontainers.image.ref.name': 'aetherpd-carrier:0.1.0'},
             'platform': {'architecture': 'arm64', 'os': 'linux'}}]}
    blobs = {helpers.sha(data): data for data in [config_bytes, manifest_bytes, layer]}
    entries = [('blobs', None, 0o755), ('blobs/sha256', None, 0o755)]
    entries += [('blobs/sha256/' + digest[7:], data, 0o644) for digest, data in sorted(blobs.items())]
    entries += [('index.json', encode(index), 0o644), ('oci-layout', b'{"imageLayoutVersion":"1.0.0"}', 0o644)]
    result = helpers.gz(helpers.tar_bytes(entries))
    return result, {'image_id': hashlib.sha256(config_bytes).hexdigest(),
                    'sha256': hashlib.sha256(result).hexdigest(), 'bytes': len(result),
                    'uncompressed_layer_bytes': len(layer_tar)}
