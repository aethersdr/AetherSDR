#!/usr/bin/env python3
"""
Publish a signed update manifest to updates.aethersdr.com, and tell whether
a release's publication is complete (RFC #5304 Phase 2;
docs/architecture/update-manifest.md).

A release counts as published only when everything a client reads is
served and verifies: releases/<tag>/manifest.json with a signature that
checks against the committed public key, and, when the tag is GitHub's
latest release, a verifying latest/ pair naming that tag or a newer one.
Anything less is incomplete, and publishing again repairs it.

latest/ is promoted only to a release at least as new as the one it names,
re-read immediately before the write. The workflow also runs this job one
at a time across tags (its `update-manifest-publish` concurrency group), so
no older run can interleave between that read and its write.

    publish_update_manifest.py validate --tag T --dir D
        refuses a manifest that is not schema 1 for T with a key_id and only
        release-download URLs (run before signing)
    publish_update_manifest.py check   --tag T --latest-tag L --public-key P
        prints "complete" or "incomplete" (reads the public site)
    publish_update_manifest.py publish --tag T --latest-tag L --public-key P --dir D
        writes D/manifest.json and D/manifest.json.sig through R2's S3 API
        (R2_ACCOUNT_ID, R2_ACCESS_KEY_ID, R2_SECRET_ACCESS_KEY), then reads
        each object back and verifies it
"""

from __future__ import annotations

import argparse
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
from build_update_manifest import DOWNLOAD_BASE, SCHEMA, version_key  # noqa: E402

BUCKET = "aethersdr-updates"
PUBLIC_BASE = "https://updates.aethersdr.com/"
RELEASE_CACHE = "public, max-age=300"   # short, so a repaired release is seen
# Cloudflare answers Python's default "Python-urllib/x.y" User-Agent with 403
# (bot protection), so the publication check names itself.
USER_AGENT = "AetherSDR-update-publisher/1 (+https://github.com/aethersdr/AetherSDR)"
LATEST_CACHE = "no-cache"


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

    def put(self, key: str, data: bytes, content_type: str, cache_control: str) -> None:
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

    def _curl(self, key: str, *args: str) -> tuple[int, bytes]:
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


def served(store, prefix: str, public_key: Path) -> tuple[bytes, dict] | None:
    """The verifying manifest served under prefix/, or None."""
    manifest = store.get(f"{prefix}/manifest.json")
    sig = store.get(f"{prefix}/manifest.json.sig")
    if manifest is None or sig is None or not verifies(manifest, sig, public_key):
        return None
    try:
        return manifest, json.loads(manifest)
    except ValueError:
        return None


def is_complete(store, tag: str, latest_tag: str, public_key: Path) -> bool:
    release = served(store, f"releases/{tag}", public_key)
    if release is None or release[1].get("tag") != tag:
        return False
    if latest_tag != tag:
        return True
    latest = served(store, "latest", public_key)
    return latest is not None and version_key(latest[1].get("tag", "v0.0.0")) >= version_key(tag)


# ── Publish ─────────────────────────────────────────────────────────────────

def validate_manifest(manifest: bytes, tag: str) -> None:
    """What the key may sign: a schema-1 manifest for `tag`, with a key_id,
    whose every URL is a release download on this repository."""
    try:
        m = json.loads(manifest)
    except ValueError:
        raise PublishError("manifest.json is not JSON") from None
    if m.get("schema") != SCHEMA:
        raise PublishError(f"schema {m.get('schema')!r}, expected {SCHEMA}")
    if m.get("tag") != tag:
        raise PublishError(f"the manifest is for {m.get('tag')!r}, not {tag}")
    if not m.get("key_id"):
        raise PublishError("no key_id: packaging/update-signing.pub was missing at build time")
    payloads = m.get("payloads") or []
    if not payloads:
        raise PublishError("no payloads")
    for p in payloads:
        for a in [p["full"], *(q["bundle"] for q in p.get("patches", []))]:
            if not a["url"].startswith(DOWNLOAD_BASE + tag + "/"):
                raise PublishError(f"{a['url']} is not a {tag} release download")

def put_pair(store, prefix: str, manifest: bytes, sig: bytes, cache: str, public_key: Path) -> None:
    store.put(f"{prefix}/manifest.json", manifest, "application/json", cache)
    store.put(f"{prefix}/manifest.json.sig", sig, "application/octet-stream", cache)
    back = served(store, prefix, public_key)
    if back is None or back[0] != manifest:
        raise PublishError(f"{prefix}/ does not read back as the manifest just written")


def publish(store, tag: str, latest_tag: str, manifest: bytes, sig: bytes,
            public_key: Path) -> list[str]:
    """Write the release's pair, then promote latest/ if this is the latest
    release and nothing newer is there. Returns the prefixes written."""
    validate_manifest(manifest, tag)
    if not verifies(manifest, sig, public_key):
        raise PublishError("the signature does not verify against the public key")
    written = [f"releases/{tag}"]
    put_pair(store, written[0], manifest, sig, RELEASE_CACHE, public_key)
    if latest_tag != tag:
        print(f"{tag} is not the latest release ({latest_tag}); latest/ is left as it is.")
        return written
    # Re-read immediately before the write: promote only over an older (or
    # this same, being repaired) release, never over a newer one.
    current = served(store, "latest", public_key)
    if current is not None and version_key(current[1].get("tag", "v0.0.0")) > version_key(tag):
        print(f"latest/ already names {current[1]['tag']}, newer than {tag}; left as it is.")
        return written
    put_pair(store, "latest", manifest, sig, LATEST_CACHE, public_key)
    written.append("latest")
    return written


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("action", choices=["validate", "check", "publish"])
    ap.add_argument("--tag", required=True)
    ap.add_argument("--latest-tag", help="GitHub's latest release tag")
    ap.add_argument("--public-key", type=Path)
    ap.add_argument("--dir", type=Path, help="holds manifest.json and manifest.json.sig")
    args = ap.parse_args(argv)
    version_key(args.tag)

    if args.action != "validate" and (args.latest_tag is None or args.public_key is None):
        ap.error(f"{args.action} needs --latest-tag and --public-key")
    try:
        if args.action == "validate":
            if args.dir is None:
                ap.error("validate needs --dir")
            validate_manifest((args.dir / "manifest.json").read_bytes(), args.tag)
            print(f"manifest.json is a valid schema-{SCHEMA} manifest for {args.tag}")
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
        written = publish(S3Store(), args.tag, args.latest_tag,
                          (args.dir / "manifest.json").read_bytes(),
                          (args.dir / "manifest.json.sig").read_bytes(), args.public_key)
        print("published and verified: " + ", ".join(f"{p}/" for p in written))
        return 0
    except PublishError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
