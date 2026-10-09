#!/usr/bin/env python3
"""Assemble the shim's container image, byte-for-byte reproducibly.

AetherSDR pins this image by SHA-256 (RFC #6271 ruling D2), so anyone must be
able to rebuild the exact bytes from this tree: a FROM-scratch OCI image
layout holding the static binary and the LICENSES bundle, with every tar
header, timestamp, owner and file order fixed and gzip's own timestamp zeroed.
No Docker daemon and no base image are involved.

    mkimage.py <binary> <LICENSES> <version> <output.tar.gz>

Prints the output's SHA-256 and size.
"""
import gzip
import hashlib
import io
import json
import sys
import tarfile

NAME = "flex-tailnet-shim"
EPOCH = 0  # every timestamp; SOURCE_DATE_EPOCH is deliberately not consulted


def sha(b: bytes) -> str:
    return "sha256:" + hashlib.sha256(b).hexdigest()


def tar_bytes(entries) -> bytes:
    """entries: [(name, data, mode)] in order; directories have data None."""
    raw = io.BytesIO()
    with tarfile.open(fileobj=raw, mode="w", format=tarfile.USTAR_FORMAT) as t:
        for name, data, mode in entries:
            ti = tarfile.TarInfo(name)
            ti.mtime, ti.uid, ti.gid, ti.uname, ti.gname, ti.mode = EPOCH, 0, 0, "", "", mode
            if data is None:
                ti.type = tarfile.DIRTYPE
                t.addfile(ti)
            else:
                ti.size = len(data)
                t.addfile(ti, io.BytesIO(data))
    return raw.getvalue()


def gz(data: bytes) -> bytes:
    out = io.BytesIO()
    with gzip.GzipFile(fileobj=out, mode="wb", mtime=EPOCH, compresslevel=9) as g:
        g.write(data)
    return out.getvalue()


def main() -> int:
    if len(sys.argv) != 5:
        print(__doc__, file=sys.stderr)
        return 2
    binary_path, licenses_path, version, out_path = sys.argv[1:]
    with open(binary_path, "rb") as f:
        binary = f.read()
    with open(licenses_path, "rb") as f:
        licenses = f.read()

    layer_tar = tar_bytes([(NAME, binary, 0o755), ("LICENSES", licenses, 0o644)])
    layer = gz(layer_tar)
    config = {
        "architecture": "arm64",
        "os": "linux",
        "config": {
            "Entrypoint": ["/" + NAME],
            "Labels": {
                "com.flexradio.waveform.name": NAME,
                "com.flexradio.waveform.version": version,
            },
        },
        "rootfs": {"type": "layers", "diff_ids": [sha(layer_tar)]},
        "history": [{"created_by": "tools/flex-tailnet-shim/mkimage.py"}],
    }
    config_b = json.dumps(config, separators=(",", ":"), sort_keys=True).encode()
    manifest = {
        "schemaVersion": 2,
        "mediaType": "application/vnd.oci.image.manifest.v1+json",
        "config": {"mediaType": "application/vnd.oci.image.config.v1+json",
                   "digest": sha(config_b), "size": len(config_b)},
        "layers": [{"mediaType": "application/vnd.oci.image.layer.v1.tar+gzip",
                    "digest": sha(layer), "size": len(layer)}],
    }
    manifest_b = json.dumps(manifest, separators=(",", ":"), sort_keys=True).encode()
    index = {
        "schemaVersion": 2,
        "mediaType": "application/vnd.oci.image.index.v1+json",
        "manifests": [{"mediaType": manifest["mediaType"], "digest": sha(manifest_b),
                       "size": len(manifest_b),
                       "annotations": {"org.opencontainers.image.ref.name": version},
                       "platform": {"architecture": "arm64", "os": "linux"}}],
    }
    index_b = json.dumps(index, separators=(",", ":"), sort_keys=True).encode()
    blobs = sorted({sha(config_b): config_b, sha(manifest_b): manifest_b, sha(layer): layer}.items())
    entries = [("blobs", None, 0o755), ("blobs/sha256", None, 0o755)]
    entries += [("blobs/sha256/" + d.split(":", 1)[1], b, 0o644) for d, b in blobs]
    entries += [("index.json", index_b, 0o644),
                ("oci-layout", b'{"imageLayoutVersion":"1.0.0"}', 0o644)]
    image = gz(tar_bytes(entries))
    with open(out_path, "wb") as f:
        f.write(image)
    print(f"{hashlib.sha256(image).hexdigest()}  {len(image)}  {out_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
