#!/usr/bin/env python3
"""
Publish a signed update manifest to updates.aethersdr.com, and tell whether
a release's publication is complete (RFC #5304 Phase 2;
docs/architecture/update-manifest.md).

Each release has two manifests (RFC #6964): manifest-v2.json, which the
bridge client reads and whose patch bundles may live on updates.aethersdr.com,
and manifest.json, the full-only GitHub-URL manifest that copies without the
bridge read. A release counts as published only when everything a client
reads is served and verifies: both releases/<tag>/ pairs with signatures that
check against the committed public key, every payload manifest-v2.json names
on updates.aethersdr.com at its recorded size, and, when the tag is GitHub's
latest release, both latest/ pairs naming that tag or a newer one. Anything
less is incomplete, and publishing again repairs it.

latest/ is promoted only to a release at least as new as the one it names,
re-read immediately before the write. The workflow also runs this job one
at a time across tags (its `update-manifest-publish` concurrency group), so
no older run can interleave between that read and its write.

    publish_update_manifest.py validate --tag T --dir D
        refuses either manifest unless it is schema 1 for T with a key_id and
        only this release's download URLs: GitHub only, and no patches, in
        manifest.json; GitHub or updates.aethersdr.com in manifest-v2.json
        (run before signing)
    publish_update_manifest.py payload-base --tag T --dir D
        prints where manifest-v2.json puts the patch bundles: github, r2 or none
    publish_update_manifest.py check   --tag T --latest-tag L --public-key P
        prints "complete" or "incomplete" (reads the public site)
    publish_update_manifest.py publish --tag T --latest-tag L --public-key P --dir D
        uploads the bundles manifest-v2.json names on updates.aethersdr.com
        from D and checks each one's SHA-256, then writes both manifests and
        their .sig through R2's S3 API (R2_ACCOUNT_ID, R2_ACCESS_KEY_ID,
        R2_SECRET_ACCESS_KEY), reading each object back and verifying it
"""

from __future__ import annotations

import argparse
import hashlib
import http.client
import json
import os
import subprocess
import sys
import tempfile
import urllib.error
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from build_update_manifest import (DOWNLOAD_BASE, LEGACY_MANIFEST, R2_BASE, SCHEMA,  # noqa: E402
                                   V2_MANIFEST, version_key)

MANIFESTS = (V2_MANIFEST, LEGACY_MANIFEST)

BUCKET = "aethersdr-updates"
PUBLIC_BASE = "https://updates.aethersdr.com/"
RELEASE_CACHE = "public, max-age=300"   # short, so a repaired release is seen
# Cloudflare answers Python's default "Python-urllib/x.y" User-Agent with 403
# (bot protection), so the publication check names itself.
USER_AGENT = "AetherSDR-update-publisher/1 (+https://github.com/aethersdr/AetherSDR)"
LATEST_CACHE = "no-cache"
# A payload under releases/<tag>/ is never rewritten with other bytes: the
# manifest pins its SHA-256.
PAYLOAD_CACHE = "public, max-age=31536000, immutable"


class PublishError(RuntimeError):
    pass


# ── Stores ──────────────────────────────────────────────────────────────────

class PublicStore:
    """Reads what clients read. Cannot write."""

    def get(self, key: str) -> bytes | None:
        req = urllib.request.Request(PUBLIC_BASE + key,
                                     headers={"Cache-Control": "no-cache", "User-Agent": USER_AGENT})
        try:
            with urllib.request.urlopen(req, timeout=30) as r:
                return r.read()
        except urllib.error.HTTPError as e:
            if e.code == 404:
                return None
            raise PublishError(f"GET {PUBLIC_BASE}{key}: HTTP {e.code}") from None
        except urllib.error.URLError as e:
            raise PublishError(f"GET {PUBLIC_BASE}{key}: {e.reason}") from None
        except (OSError, http.client.HTTPException) as e:
            # A timeout or a dropped connection after the request was sent
            # surfaces unwrapped (TimeoutError, RemoteDisconnected,
            # IncompleteRead).
            raise PublishError(f"GET {PUBLIC_BASE}{key}: {type(e).__name__}: {e}") from None

    def size(self, key: str) -> int | None:
        """The served object's length, from a HEAD, or None if absent."""
        req = urllib.request.Request(PUBLIC_BASE + key, method="HEAD",
                                     headers={"Cache-Control": "no-cache", "User-Agent": USER_AGENT})
        try:
            with urllib.request.urlopen(req, timeout=30) as r:
                length = r.headers.get("Content-Length")
                return int(length) if length is not None else None
        except urllib.error.HTTPError as e:
            if e.code == 404:
                return None
            raise PublishError(f"HEAD {PUBLIC_BASE}{key}: HTTP {e.code}") from None
        except urllib.error.URLError as e:
            raise PublishError(f"HEAD {PUBLIC_BASE}{key}: {e.reason}") from None
        except (OSError, http.client.HTTPException, ValueError) as e:
            raise PublishError(f"HEAD {PUBLIC_BASE}{key}: {type(e).__name__}: {e}") from None

    def put(self, key: str, data: bytes, content_type: str, cache_control: str) -> None:
        raise PublishError("the public store is read-only")

    def put_file(self, key: str, path: Path, content_type: str, cache_control: str) -> None:
        raise PublishError("the public store is read-only")


class S3Store:
    """R2 through its S3 API, signed by curl (--aws-sigv4). The credentials
    are a token scoped to this one bucket."""

    def __init__(self) -> None:
        try:
            self.account = os.environ["R2_ACCOUNT_ID"]
            self.user = os.environ["R2_ACCESS_KEY_ID"] + ":" + os.environ["R2_SECRET_ACCESS_KEY"]
        except KeyError as e:
            raise PublishError(f"missing {e.args[0]}") from None
        if any(ord(c) < 0x20 or ord(c) == 0x7F for c in self.user):
            raise PublishError("the R2 credentials hold a control character")

    def _curl(self, key: str, *args: str, digest: bool = False) -> tuple[int, bytes]:
        """digest: the body is not returned but hashed as it is read back,
        so a large object is never held in memory; the result is then the
        SHA-256 hex and the size, as "<hex> <size>"."""
        # The credentials go to curl through a config file only this user can
        # read (NamedTemporaryFile is 0600), never on its command line, where
        # any process could read them for the life of the request.
        with tempfile.NamedTemporaryFile() as body, \
                tempfile.NamedTemporaryFile("w", suffix=".curlrc") as config:
            quoted = self.user.replace("\\", "\\\\").replace('"', '\\"')
            config.write(f'user = "{quoted}"\n')
            config.flush()
            out = subprocess.run(
                # -q first: no ~/.curlrc is read, only this config.
                ["curl", "-q", "-sS", "--retry", "3", "--aws-sigv4", "aws:amz:auto:s3",
                 "-K", config.name, "-o", body.name, "-w", "%{http_code}", *args,
                 f"https://{self.account}.r2.cloudflarestorage.com/{BUCKET}/{key}"],
                capture_output=True, text=True)
            if out.returncode != 0:
                raise PublishError(f"{key}: curl failed: {out.stderr.strip()}")
            if digest:
                hex_, size = file_digest(Path(body.name))
                return int(out.stdout or 0), f"{hex_} {size}".encode()
            return int(out.stdout or 0), Path(body.name).read_bytes()

    def get(self, key: str) -> bytes | None:
        code, data = self._curl(key)
        if code == 404:
            return None
        if code != 200:
            raise PublishError(f"GET {key}: HTTP {code}")
        return data

    def put(self, key: str, data: bytes, content_type: str, cache_control: str) -> None:
        with tempfile.NamedTemporaryFile() as f:
            f.write(data)
            f.flush()
            code, _ = self._curl(key, "-X", "PUT", "-H", f"Content-Type: {content_type}",
                                 "-H", f"Cache-Control: {cache_control}",
                                 "--data-binary", f"@{f.name}")
        if code != 200:
            raise PublishError(f"PUT {key}: HTTP {code}")

    def put_file(self, key: str, path: Path, content_type: str, cache_control: str) -> None:
        """Stream `path` up (curl -T), never reading it into memory. The
        request is signed over the file's SHA-256, so R2 refuses an upload
        whose bytes arrive changed."""
        hex_, _ = file_digest(path)
        code, _ = self._curl(key, "-T", str(path), "-H", f"Content-Type: {content_type}",
                             "-H", f"Cache-Control: {cache_control}",
                             "-H", f"x-amz-content-sha256: {hex_}")
        if code != 200:
            raise PublishError(f"PUT {key}: HTTP {code}")

    def digest(self, key: str) -> tuple[str, int] | None:
        """The stored object's SHA-256 hex and size, or None if absent."""
        code, data = self._curl(key, digest=True)
        if code == 404:
            return None
        if code != 200:
            raise PublishError(f"GET {key}: HTTP {code}")
        hex_, size = data.decode().split()
        return hex_, int(size)


def file_digest(path: Path) -> tuple[str, int]:
    h = hashlib.sha256()
    size = 0
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
            size += len(chunk)
    return h.hexdigest(), size


# ── Checks ──────────────────────────────────────────────────────────────────

def verifies(manifest: bytes, sig: bytes, public_key: Path) -> bool:
    if len(sig) != 64:
        return False
    with tempfile.TemporaryDirectory() as d:
        m, s = Path(d) / "m", Path(d) / "s"
        m.write_bytes(manifest)
        s.write_bytes(sig)
        r = subprocess.run(["openssl", "pkeyutl", "-verify", "-rawin", "-pubin",
                            "-inkey", str(public_key), "-in", str(m), "-sigfile", str(s)],
                           capture_output=True)
        return r.returncode == 0


def served(store, prefix: str, public_key: Path,
           name: str = LEGACY_MANIFEST) -> tuple[bytes, dict] | None:
    """The verifying manifest `name` served under prefix/, or None."""
    manifest = store.get(f"{prefix}/{name}")
    sig = store.get(f"{prefix}/{name}.sig")
    if manifest is None or sig is None or not verifies(manifest, sig, public_key):
        return None
    try:
        return manifest, json.loads(manifest)
    except ValueError:
        return None


def r2_assets(m: dict, tag: str) -> list[dict]:
    """The assets a manifest names on updates.aethersdr.com for `tag`."""
    out = []
    for p in m.get("payloads") or []:
        for a in [p["full"], *(q["bundle"] for q in p.get("patches", []))]:
            if a["url"].startswith(R2_BASE + tag + "/"):
                out.append(a)
    return out


def is_complete(store, tag: str, latest_tag: str, public_key: Path) -> bool:
    for name in MANIFESTS:
        release = served(store, f"releases/{tag}", public_key, name)
        if release is None or release[1].get("tag") != tag:
            return False
        if name == V2_MANIFEST:
            for a in r2_assets(release[1], tag):
                if store.size(a["url"][len(PUBLIC_BASE):]) != a["size"]:
                    return False
    if latest_tag != tag:
        return True
    for name in MANIFESTS:
        latest = served(store, "latest", public_key, name)
        if latest is None or version_key(latest[1].get("tag", "v0.0.0")) < version_key(tag):
            return False
    return True


# ── Publish ─────────────────────────────────────────────────────────────────

def one_file_under(url: str, base: str, tag: str) -> bool:
    """`url` is exactly <base><tag>/<one file>, the shape the client accepts."""
    prefix = base + tag + "/"
    rest = url[len(prefix):]
    return (url.startswith(prefix) and rest != ""
            and not any(c in rest for c in "/?#\\") and ".." not in rest)


def validate_manifest(manifest: bytes, tag: str, name: str = LEGACY_MANIFEST) -> None:
    """What the key may sign: a schema-1 manifest for `tag`, with a key_id,
    whose every URL is one file of this release. manifest.json is read by
    copies that accept only GitHub, so its URLs must be GitHub's and it may
    carry no patches; manifest-v2.json may also name updates.aethersdr.com."""
    if name not in MANIFESTS:
        raise PublishError(f"{name} is not a manifest this tool publishes")
    try:
        m = json.loads(manifest)
    except ValueError:
        raise PublishError(f"{name} is not JSON") from None
    if m.get("schema") != SCHEMA:
        raise PublishError(f"schema {m.get('schema')!r}, expected {SCHEMA}")
    if m.get("tag") != tag:
        raise PublishError(f"the manifest is for {m.get('tag')!r}, not {tag}")
    if not m.get("key_id"):
        raise PublishError("no key_id: packaging/update-signing.pub was missing at build time")
    payloads = m.get("payloads") or []
    if not payloads:
        raise PublishError("no payloads")
    bases = (DOWNLOAD_BASE,) if name == LEGACY_MANIFEST else (DOWNLOAD_BASE, R2_BASE)
    for p in payloads:
        if name == LEGACY_MANIFEST and p.get("patches"):
            raise PublishError(f"{name} carries patches; it must list full downloads only")
        for a in [p["full"], *(q["bundle"] for q in p.get("patches", []))]:
            if not any(one_file_under(a["url"], b, tag) for b in bases):
                raise PublishError(f"{a['url']} is not a {tag} download {name} may name")


def payload_base(m: dict) -> str:
    """Where a manifest's patch bundles are: "github", "r2", or "none"."""
    urls = [q["bundle"]["url"] for p in m.get("payloads") or [] for q in p.get("patches", [])]
    if not urls:
        return "none"
    if all(u.startswith(R2_BASE) for u in urls):
        return "r2"
    if all(u.startswith(DOWNLOAD_BASE) for u in urls):
        return "github"
    raise PublishError("the patch bundles are split between GitHub and updates.aethersdr.com")


def put_pair(store, prefix: str, name: str, manifest: bytes, sig: bytes, cache: str,
             public_key: Path) -> None:
    store.put(f"{prefix}/{name}", manifest, "application/json", cache)
    store.put(f"{prefix}/{name}.sig", sig, "application/octet-stream", cache)
    back = served(store, prefix, public_key, name)
    if back is None or back[0] != manifest:
        raise PublishError(f"{prefix}/{name} does not read back as the manifest just written")


def upload_payloads(store, tag: str, m: dict, payload_dir: Path | None) -> list[str]:
    """Put every asset `m` names on updates.aethersdr.com, from payload_dir,
    and check what is stored is what the manifest pins. Before any manifest
    is written, so no published manifest names a file that is not there."""
    keys = []
    for a in r2_assets(m, tag):
        key = a["url"][len(PUBLIC_BASE):]
        src = (payload_dir / a["asset"]) if payload_dir is not None else None
        if src is None or not src.is_file():
            raise PublishError(f"{a['asset']}: named on updates.aethersdr.com but not in --dir")
        if file_digest(src) != (a["sha256"], a["size"]):
            raise PublishError(f"{a['asset']}: does not match the manifest")
        store.put_file(key, src, "application/x-tar", PAYLOAD_CACHE)
        if store.digest(key) != (a["sha256"], a["size"]):
            raise PublishError(f"{key}: does not read back as the file just uploaded")
        keys.append(key)
    return keys


def publish(store, tag: str, latest_tag: str, pairs: dict[str, tuple[bytes, bytes]],
            public_key: Path, payload_dir: Path | None = None) -> list[str]:
    """Upload manifest-v2's R2 payloads, write the release's two pairs, then
    promote both latest/ pairs if this is the latest release and nothing
    newer is there. Returns the prefixes written."""
    if set(pairs) != set(MANIFESTS):
        raise PublishError(f"publish needs exactly {', '.join(MANIFESTS)}")
    parsed = {}
    for name in MANIFESTS:
        manifest, sig = pairs[name]
        validate_manifest(manifest, tag, name)
        if not verifies(manifest, sig, public_key):
            raise PublishError(f"{name}: the signature does not verify against the public key")
        parsed[name] = json.loads(manifest)
    upload_payloads(store, tag, parsed[V2_MANIFEST], payload_dir)
    written = [f"releases/{tag}"]
    for name in MANIFESTS:
        put_pair(store, written[0], name, *pairs[name], RELEASE_CACHE, public_key)
    if latest_tag != tag:
        print(f"{tag} is not the latest release ({latest_tag}); latest/ is left as it is.")
        return written
    # Re-read immediately before the write: promote only over an older (or
    # this same, being repaired) release, never over a newer one. Either
    # latest/ manifest naming a newer release blocks both, so the two never
    # name different releases because of this run.
    for name in MANIFESTS:
        current = served(store, "latest", public_key, name)
        if current is not None and version_key(current[1].get("tag", "v0.0.0")) > version_key(tag):
            print(f"latest/{name} already names {current[1]['tag']}, newer than {tag}; left as it is.")
            return written
    for name in MANIFESTS:
        put_pair(store, "latest", name, *pairs[name], LATEST_CACHE, public_key)
    written.append("latest")
    return written


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("action", choices=["validate", "payload-base", "check", "publish"])
    ap.add_argument("--tag", required=True)
    ap.add_argument("--latest-tag", help="GitHub's latest release tag")
    ap.add_argument("--public-key", type=Path)
    ap.add_argument("--dir", type=Path,
                    help="holds manifest.json, manifest-v2.json, their .sig and any R2 bundles")
    args = ap.parse_args(argv)
    version_key(args.tag)

    if args.action in ("check", "publish") and (args.latest_tag is None or args.public_key is None):
        ap.error(f"{args.action} needs --latest-tag and --public-key")
    try:
        if args.action == "validate":
            if args.dir is None:
                ap.error("validate needs --dir")
            for name in MANIFESTS:
                validate_manifest((args.dir / name).read_bytes(), args.tag, name)
                print(f"{name} is a valid schema-{SCHEMA} manifest for {args.tag}")
            return 0
        if args.action == "payload-base":
            if args.dir is None:
                ap.error("payload-base needs --dir")
            try:
                m = json.loads((args.dir / V2_MANIFEST).read_bytes())
            except ValueError:
                raise PublishError(f"{V2_MANIFEST} is not JSON") from None
            print(payload_base(m))
            return 0
        if args.action == "check":
            # A read that fails is not proof of publication: "incomplete", so
            # the run before publishing goes on to publish (which repairs),
            # and the check after it fails the job.
            try:
                complete = is_complete(PublicStore(), args.tag, args.latest_tag, args.public_key)
            except PublishError as e:
                print(f"warning: {e}", file=sys.stderr)
                complete = False
            print("complete" if complete else "incomplete")
            return 0
        if args.dir is None:
            ap.error("publish needs --dir")
        pairs = {name: ((args.dir / name).read_bytes(), (args.dir / f"{name}.sig").read_bytes())
                 for name in MANIFESTS}
        written = publish(S3Store(), args.tag, args.latest_tag, pairs, args.public_key,
                          payload_dir=args.dir)
        print("published and verified: " + ", ".join(f"{p}/" for p in written))
        return 0
    except PublishError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
