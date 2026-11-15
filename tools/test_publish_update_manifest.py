#!/usr/bin/env python3
"""Tests for tools/publish_update_manifest.py, against an in-memory store
and a real (ephemeral) Ed25519 key. No network, no R2 credentials."""

from __future__ import annotations

import contextlib
import hashlib
import http.client
import io
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
import urllib.error
from unittest import mock
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import publish_update_manifest as pub  # noqa: E402

REPO = Path(__file__).resolve().parent.parent


V2, LEGACY = pub.V2_MANIFEST, pub.LEGACY_MANIFEST


class FakeStore:
    def __init__(self) -> None:
        self.objects: dict[str, bytes] = {}
        self.cache: dict[str, str] = {}
        self.fail_on: str | None = None   # a key whose PUT fails
        self.corrupt: str | None = None   # a key stored with other bytes

    def get(self, key: str) -> bytes | None:
        return self.objects.get(key)

    def size(self, key: str) -> int | None:
        return len(self.objects[key]) if key in self.objects else None

    def digest(self, key: str) -> tuple[str, int] | None:
        if key not in self.objects:
            return None
        return hashlib.sha256(self.objects[key]).hexdigest(), len(self.objects[key])

    def put(self, key: str, data: bytes, content_type: str, cache_control: str) -> None:
        if key == self.fail_on:
            raise pub.PublishError(f"PUT {key}: HTTP 500")
        self.objects[key] = data
        self.cache[key] = cache_control

    def put_file(self, key: str, path: Path, content_type: str, cache_control: str) -> None:
        self.put(key, b"changed" if key == self.corrupt else path.read_bytes(), content_type,
                 cache_control)


class Base(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="pub-test-"))
        self.key = self.tmp / "k.pem"
        self.pub = self.tmp / "k.pub"
        subprocess.run(["openssl", "genpkey", "-algorithm", "ed25519", "-out", str(self.key)], check=True)
        subprocess.run(["openssl", "pkey", "-in", str(self.key), "-pubout", "-out", str(self.pub)], check=True)
        self.store = FakeStore()

    def tearDown(self) -> None:
        shutil.rmtree(self.tmp, ignore_errors=True)

    BUNDLE = b"bundle bytes" * 100

    @classmethod
    def body(cls, release: str, name: str = V2, bundle_base: str = pub.DOWNLOAD_BASE,
             **override) -> dict:
        """manifest-v2.json with one patch on bundle_base, or (name=LEGACY)
        what it implies: the same patch on GitHub, none once on R2."""
        base = pub.DOWNLOAD_BASE + release + "/"
        bundle = {"asset": "AetherSDR-x.update.tar", "url": bundle_base + release + "/AetherSDR-x.update.tar",
                  "sha256": hashlib.sha256(cls.BUNDLE).hexdigest(), "size": len(cls.BUNDLE)}
        m = {"schema": 1, "tag": release, "version": release[1:], "key_id": "788fb6e31f7ecba4",
             "payloads": [{"platform": "windows", "arch": "x86_64",
                           "full": {"url": base + "AetherSDR-x.zip"},
                           "patches": ([] if name == LEGACY and bundle_base != pub.DOWNLOAD_BASE
                                       else [{"bundle": bundle}])}]}
        m.update(override)
        return m

    def sign(self, manifest: bytes, key: Path | None = None) -> bytes:
        m = self.tmp / "m.json"
        m.write_bytes(manifest)
        sig = self.tmp / "m.sig"
        subprocess.run(["openssl", "pkeyutl", "-sign", "-rawin", "-inkey", str(key or self.key),
                        "-in", str(m), "-out", str(sig)], check=True)
        return sig.read_bytes()

    def signed(self, tag: str, key: Path | None = None,
               bundle_base: str = pub.DOWNLOAD_BASE) -> dict[str, tuple[bytes, bytes]]:
        """Both manifests of one release, each with its signature."""
        pairs = {}
        for name in pub.MANIFESTS:
            manifest = json.dumps(self.body(tag, name, bundle_base)).encode()
            pairs[name] = (manifest, self.sign(manifest, key))
        return pairs

    def payload_dir(self) -> Path:
        d = self.tmp / "payloads"
        d.mkdir(exist_ok=True)
        (d / "AetherSDR-x.update.tar").write_bytes(self.BUNDLE)
        return d

    def latest_tag(self, name: str = LEGACY) -> str | None:
        raw = self.store.objects.get(f"latest/{name}")
        return json.loads(raw)["tag"] if raw else None


class Completeness(Base):
    def test_publish_then_complete(self) -> None:
        pairs = self.signed("v26.10.4")
        self.assertFalse(pub.is_complete(self.store, "v26.10.4", "v26.10.4", self.pub))
        self.assertEqual(pub.publish(self.store, "v26.10.4", "v26.10.4", pairs, self.pub),
                         ["releases/v26.10.4", "latest"])
        self.assertTrue(pub.is_complete(self.store, "v26.10.4", "v26.10.4", self.pub))

    def test_manifest_without_signature_is_incomplete_and_repaired(self) -> None:
        """The partial publish the review reproduced: the manifest PUT
        succeeded, the signature PUT failed."""
        pairs = self.signed("v26.10.4")
        self.store.fail_on = "releases/v26.10.4/manifest.json.sig"
        with self.assertRaises(pub.PublishError):
            pub.publish(self.store, "v26.10.4", "v26.10.4", pairs, self.pub)
        self.assertIn("releases/v26.10.4/manifest.json", self.store.objects)
        self.assertFalse(pub.is_complete(self.store, "v26.10.4", "v26.10.4", self.pub),
                         "a served manifest with no signature is not a publication")
        self.store.fail_on = None
        pub.publish(self.store, "v26.10.4", "v26.10.4", pairs, self.pub)
        self.assertTrue(pub.is_complete(self.store, "v26.10.4", "v26.10.4", self.pub))

    def test_release_without_latest_is_incomplete_when_it_is_latest(self) -> None:
        pairs = self.signed("v26.10.4")
        self.store.fail_on = "latest/manifest.json"
        with self.assertRaises(pub.PublishError):
            pub.publish(self.store, "v26.10.4", "v26.10.4", pairs, self.pub)
        self.assertFalse(pub.is_complete(self.store, "v26.10.4", "v26.10.4", self.pub))
        self.assertTrue(pub.is_complete(self.store, "v26.10.4", "v26.10.5", self.pub),
                        "an older release needs only its own pair")
        self.store.fail_on = None
        pub.publish(self.store, "v26.10.4", "v26.10.4", pairs, self.pub)
        self.assertTrue(pub.is_complete(self.store, "v26.10.4", "v26.10.4", self.pub))

    def test_a_signature_that_does_not_verify_is_incomplete(self) -> None:
        pairs = self.signed("v26.10.4")
        for name, (m, s) in pairs.items():
            self.store.objects[f"releases/v26.10.4/{name}"] = m
            self.store.objects[f"releases/v26.10.4/{name}.sig"] = s
        self.store.objects["releases/v26.10.4/manifest.json.sig"] = bytes(64)
        self.assertFalse(pub.is_complete(self.store, "v26.10.4", "v26.10.5", self.pub))

    def test_wrong_key_is_refused_before_any_write(self) -> None:
        other = self.tmp / "other.pem"
        subprocess.run(["openssl", "genpkey", "-algorithm", "ed25519", "-out", str(other)], check=True)
        pairs = self.signed("v26.10.4", key=other)
        with self.assertRaises(pub.PublishError):
            pub.publish(self.store, "v26.10.4", "v26.10.4", pairs, self.pub)
        self.assertEqual(self.store.objects, {})


class Validation(Base):
    def test_only_a_manifest_for_this_release_may_be_signed(self) -> None:
        pub.validate_manifest(json.dumps(self.body("v26.10.4")).encode(), "v26.10.4", V2)
        pub.validate_manifest(json.dumps(self.body("v26.10.4", LEGACY)).encode(), "v26.10.4", LEGACY)
        bad = [
            self.body("v26.10.4", tag="v26.10.3"),       # another release's tag, this release's URLs
            self.body("v26.10.4", schema=2),
            self.body("v26.10.4", key_id=""),
            self.body("v26.10.4", payloads=[]),
        ]
        foreign = self.body("v26.10.4")
        foreign["payloads"][0]["patches"][0]["bundle"]["url"] = "https://evil.example/x.update.tar"
        older = self.body("v26.10.4")
        older["payloads"][0]["full"]["url"] = pub.DOWNLOAD_BASE + "v26.10.3/AetherSDR-x.zip"
        for m in bad + [foreign, older]:
            for name in pub.MANIFESTS:
                with self.subTest(name=name), self.assertRaises(pub.PublishError):
                    pub.validate_manifest(json.dumps(m).encode(), "v26.10.4", name)
        with self.assertRaises(pub.PublishError):
            pub.validate_manifest(b"not json", "v26.10.4", V2)
        with self.assertRaises(pub.PublishError):
            pub.validate_manifest(json.dumps(self.body("v26.10.4")).encode(), "v26.10.4", "other.json")


class PublicRead(Base):
    class Response:
        def __enter__(self):
            return self

        def __exit__(self, *a):
            return False

        def read(self):
            return b"x"

    def test_the_public_check_names_itself(self) -> None:
        """Cloudflare refused Python's default User-Agent with 403; the first
        real publish's check failed on exactly that."""
        seen = []
        with mock.patch.object(pub.urllib.request, "urlopen",
                               lambda req, timeout=None: (seen.append(req), self.Response())[1]):
            self.assertEqual(pub.PublicStore().get("latest/manifest.json"), b"x")
        ua = seen[0].get_header("User-agent")
        self.assertTrue(ua and ua.startswith("AetherSDR-"), ua)
        self.assertEqual(seen[0].get_header("Cache-control"), "no-cache")

    def test_a_refusal_is_an_error_not_absence(self) -> None:
        """Only 404 means "not published"; anything else is a PublishError
        with a message, never a traceback, never "absent"."""
        cases = [
            (urllib.error.HTTPError("u", 404, "Not Found", {}, None), None),
            (urllib.error.HTTPError("u", 403, "Forbidden", {}, None), "HTTP 403"),
            (urllib.error.HTTPError("u", 503, "Unavailable", {}, None), "HTTP 503"),
            (urllib.error.URLError("no route"), "no route"),
            (TimeoutError("timed out"), "TimeoutError"),
            (http.client.RemoteDisconnected("closed"), "RemoteDisconnected"),
            (http.client.IncompleteRead(b"x", 5), "IncompleteRead"),
        ]
        for err, expect in cases:
            with self.subTest(err=type(err).__name__), \
                    mock.patch.object(pub.urllib.request, "urlopen", side_effect=err):
                if expect is None:
                    self.assertIsNone(pub.PublicStore().get("latest/manifest.json"))
                else:
                    with self.assertRaises(pub.PublishError) as ctx:
                        pub.PublicStore().get("latest/manifest.json")
                    self.assertIn(expect, str(ctx.exception))

    def test_size_without_content_length_reads_the_range_total(self) -> None:
        """An edge that answers HEAD with no Content-Length (chunked or
        compressed framing) is asked for one byte, and Content-Range gives
        the total. Only if both are missing is the size unknown."""
        def answer(head: dict, ranged: dict):
            seen = []

            class R:
                def __init__(self, h):
                    self.headers = h

                def __enter__(self):
                    return self

                def __exit__(self, *a):
                    return False

            def urlopen(req, timeout=None):
                seen.append((req.get_method(), req.get_header("Range")))
                return R(head if req.get_method() == "HEAD" else ranged)
            return urlopen, seen

        urlopen, seen = answer({"Content-Length": "1200"}, {})
        with mock.patch.object(pub.urllib.request, "urlopen", urlopen):
            self.assertEqual(pub.PublicStore().size("releases/v1/x.tar"), 1200)
        self.assertEqual(seen, [("HEAD", None)])
        urlopen, seen = answer({}, {"Content-Range": "bytes 0-0/1200"})
        with mock.patch.object(pub.urllib.request, "urlopen", urlopen):
            self.assertEqual(pub.PublicStore().size("releases/v1/x.tar"), 1200)
        self.assertEqual(seen, [("HEAD", None), ("GET", "bytes=0-0")])
        for ranged in ({}, {"Content-Range": "bytes 0-0/*"}):
            urlopen, _ = answer({}, ranged)
            with mock.patch.object(pub.urllib.request, "urlopen", urlopen):
                self.assertIsNone(pub.PublicStore().size("releases/v1/x.tar"))
        urlopen, _ = answer({"Content-Length": "lots"}, {})
        with mock.patch.object(pub.urllib.request, "urlopen", urlopen), self.assertRaises(pub.PublishError):
            pub.PublicStore().size("releases/v1/x.tar")

    def test_check_reports_a_failed_read_as_incomplete(self) -> None:
        """Before publishing, a failed read means "publish" (which repairs),
        not a dead job; after publishing, "incomplete" fails the check."""
        out = io.StringIO()
        with mock.patch.object(pub.urllib.request, "urlopen", side_effect=TimeoutError("slow edge")), \
                contextlib.redirect_stdout(out), contextlib.redirect_stderr(io.StringIO()) as err:
            rc = pub.main(["check", "--tag", "v26.10.4", "--latest-tag", "v26.10.4",
                           "--public-key", str(self.pub)])
        self.assertEqual(rc, 0)
        self.assertEqual(out.getvalue().strip(), "incomplete")
        self.assertIn("slow edge", err.getvalue())


class S3Credentials(Base):
    ENV = {"R2_ACCOUNT_ID": "acct", "R2_ACCESS_KEY_ID": "keyid", "R2_SECRET_ACCESS_KEY": "s3cr3t"}

    def run_get(self, env: dict) -> list:
        """S3Store().get with curl replaced: what curl was given."""
        calls = []

        def run(argv, **kw):
            config = Path(argv[argv.index("-K") + 1])
            calls.append({"argv": list(argv), "config": config,
                          "text": config.read_text(), "mode": config.stat().st_mode & 0o777})
            return mock.Mock(returncode=0, stdout="404", stderr="")

        with mock.patch.dict(os.environ, env), mock.patch.object(pub.subprocess, "run", run):
            self.assertIsNone(pub.S3Store().get("latest/manifest.json"))
        return calls

    def test_the_secret_never_reaches_curls_command_line(self) -> None:
        (call,) = self.run_get(self.ENV)
        self.assertEqual(call["argv"][:2], ["curl", "-q"], "no ~/.curlrc: -q comes first")
        self.assertFalse(any("s3cr3t" in a for a in call["argv"]), call["argv"])
        self.assertIn('user = "keyid:s3cr3t"', call["text"])
        self.assertEqual(call["mode"], 0o600)
        self.assertFalse(call["config"].exists(), "the config is deleted after the call")

    def test_quotes_and_backslashes_are_escaped(self) -> None:
        (call,) = self.run_get({**self.ENV, "R2_SECRET_ACCESS_KEY": 'a"b\\c'})
        self.assertIn('user = "keyid:a\\"b\\\\c"', call["text"])

    def test_put_file_streams_and_signs_the_payload_hash(self) -> None:
        """A 372 MB file goes up with curl -T, never through memory, and the
        request is signed over its SHA-256 so R2 refuses changed bytes."""
        f = self.tmp / "big.tar"
        f.write_bytes(b"payload" * 1000)
        calls = []

        def run(argv, **kw):
            calls.append(list(argv))
            return mock.Mock(returncode=0, stdout="200", stderr="")

        with mock.patch.dict(os.environ, self.ENV), mock.patch.object(pub.subprocess, "run", run):
            pub.S3Store().put_file("releases/v26.10.4/big.tar", f, "application/x-tar", pub.PAYLOAD_CACHE)
        (argv,) = calls
        self.assertEqual(argv[argv.index("-T") + 1], str(f))
        self.assertNotIn("--data-binary", argv)
        self.assertIn("x-amz-content-sha256: " + hashlib.sha256(f.read_bytes()).hexdigest(), argv)
        self.assertIn("Cache-Control: " + pub.PAYLOAD_CACHE, argv)
        self.assertTrue(argv[-1].endswith("/aethersdr-updates/releases/v26.10.4/big.tar"))

    def test_control_characters_are_refused(self) -> None:
        for bad in ("a\nb", "a\rb", "a\tb", "a\x7fb"):   # an env var cannot hold NUL
            with self.subTest(bad=repr(bad)), mock.patch.dict(os.environ, {**self.ENV,
                                                                       "R2_SECRET_ACCESS_KEY": bad}):
                with self.assertRaises(pub.PublishError):
                    pub.S3Store()


class TwoManifests(Base):
    """RFC #6964: manifest-v2.json for the bridge client, manifest.json
    (GitHub only; full downloads only once the bundles are on R2) for copies
    without it."""

    R2 = pub.R2_BASE

    def test_legacy_names_only_github(self) -> None:
        r2_full = self.body("v26.10.4", LEGACY)
        r2_full["payloads"][0]["full"]["url"] = self.R2 + "v26.10.4/AetherSDR-x.zip"
        for m in (r2_full, self.body("v26.10.4", V2, self.R2)):   # R2 full; R2 patch
            with self.assertRaises(pub.PublishError):
                pub.validate_manifest(json.dumps(m).encode(), "v26.10.4", LEGACY)
        # GitHub patches are what v26.11.1 and v26.11.2 already apply.
        pub.validate_manifest(json.dumps(self.body("v26.10.4", LEGACY)).encode(), "v26.10.4", LEGACY)

    def test_legacy_is_what_v2_implies(self) -> None:
        enc = lambda m: json.dumps(m).encode()   # noqa: E731
        gh_v2, gh_legacy = self.body("v26.10.4", V2), self.body("v26.10.4", LEGACY)
        r2_v2, r2_legacy = self.body("v26.10.4", V2, self.R2), self.body("v26.10.4", LEGACY, self.R2)
        self.assertEqual(gh_legacy["payloads"][0]["patches"], gh_v2["payloads"][0]["patches"])
        self.assertEqual(r2_legacy["payloads"][0]["patches"], [])
        pub.validate_pair(enc(gh_legacy), enc(gh_v2), "v26.10.4")
        pub.validate_pair(enc(r2_legacy), enc(r2_v2), "v26.10.4")
        dropped = self.body("v26.10.4", LEGACY)
        dropped["payloads"][0]["patches"] = []          # v2's GitHub patches left out
        kept = self.body("v26.10.4", LEGACY)           # GitHub patches while v2 is on R2
        other = self.body("v26.10.4", LEGACY)
        other["payloads"][0]["patches"][0]["bundle"]["sha256"] = "0" * 64
        for legacy, v2 in ((dropped, gh_v2), (kept, r2_v2), (other, gh_v2), (r2_v2, r2_v2)):
            with self.assertRaises(pub.PublishError):
                pub.validate_pair(enc(legacy), enc(v2), "v26.10.4")
        pairs = self.signed("v26.10.4", bundle_base=self.R2)
        pairs[LEGACY] = (enc(kept), self.sign(enc(kept)))
        with self.assertRaises(pub.PublishError):
            pub.publish(self.store, "v26.10.4", "v26.10.4", pairs, self.pub, payload_dir=self.payload_dir())
        self.assertEqual(self.store.objects, {}, "a mismatched pair writes nothing")

    def test_urls_are_checked_decoded_like_the_client(self) -> None:
        for base in (pub.DOWNLOAD_BASE, self.R2):
            host = base.split("/")[2]
            rel = base.split(host, 1)[1]
            good = base + "v26.10.4/AetherSDR-x.update.tar"
            self.assertTrue(pub.one_file_under(good, base, "v26.10.4"), good)
            for url in [base + "v26.10.4/sub%2fpatch.update.tar",
                        base + "v26.10.4/sub%2Fpatch.update.tar",
                        base + "v26.10.4/%2e%2e",
                        base + "v26.10.4/..%2Fx",
                        base + "v26.10.4/x%5cy.update.tar",
                        f"https://{host}:8443{rel}v26.10.4/AetherSDR-x.update.tar",
                        f"https://u@{host}{rel}v26.10.4/AetherSDR-x.update.tar",
                        f"https://u:p@{host}{rel}v26.10.4/AetherSDR-x.update.tar",
                        f"https://{host}.evil.example{rel}v26.10.4/AetherSDR-x.update.tar",
                        f"http://{host}{rel}v26.10.4/AetherSDR-x.update.tar",
                        good + "?x=1", good + "?", good + "#f", good + "#",
                        base + "v26.10.3/AetherSDR-x.update.tar",
                        base + "v26.10.4/"]:
                with self.subTest(url=url):
                    self.assertFalse(pub.one_file_under(url, base, "v26.10.4"))
            self.assertFalse(pub.one_file_under(good, base, ""), "no tag, no trust")

    def test_v2_may_name_r2_with_the_clients_rules(self) -> None:
        pub.validate_manifest(json.dumps(self.body("v26.10.4", V2, self.R2)).encode(), "v26.10.4", V2)
        for url in [self.R2 + "v26.10.3/AetherSDR-x.update.tar",          # another release
                    self.R2 + "v26.10.4/sub/AetherSDR-x.update.tar",      # two segments
                    self.R2 + "v26.10.4/AetherSDR-x.update.tar?x=1",
                    self.R2 + "v26.10.4/AetherSDR-x.update.tar#f",
                    self.R2 + "v26.10.4/",
                    self.R2 + "v26.10.4/..",
                    "https://updates.aethersdr.com.evil.example/releases/v26.10.4/AetherSDR-x.update.tar",
                    "http://updates.aethersdr.com/releases/v26.10.4/AetherSDR-x.update.tar",
                    pub.DOWNLOAD_BASE + "v26.10.4/sub/AetherSDR-x.update.tar"]:
            m = self.body("v26.10.4", V2)
            m["payloads"][0]["patches"][0]["bundle"]["url"] = url
            with self.subTest(url=url), self.assertRaises(pub.PublishError):
                pub.validate_manifest(json.dumps(m).encode(), "v26.10.4", V2)

    def test_r2_bundles_are_uploaded_and_checked_before_any_manifest(self) -> None:
        pairs = self.signed("v26.10.4", bundle_base=self.R2)
        key = "releases/v26.10.4/AetherSDR-x.update.tar"
        pub.publish(self.store, "v26.10.4", "v26.10.4", pairs, self.pub, payload_dir=self.payload_dir())
        self.assertEqual(self.store.objects[key], self.BUNDLE)
        self.assertEqual(self.store.cache[key], pub.PAYLOAD_CACHE)
        self.assertNotIn("immutable", pub.PAYLOAD_CACHE, "a repair may rewrite a payload")
        self.assertTrue(pub.is_complete(self.store, "v26.10.4", "v26.10.4", self.pub))
        del self.store.objects[key]
        self.assertFalse(pub.is_complete(self.store, "v26.10.4", "v26.10.4", self.pub),
                         "a manifest naming a bundle R2 does not serve is not a publication")

    def test_a_missing_or_changed_bundle_publishes_no_manifest(self) -> None:
        pairs = self.signed("v26.10.4", bundle_base=self.R2)
        with self.assertRaises(pub.PublishError):
            pub.publish(self.store, "v26.10.4", "v26.10.4", pairs, self.pub)   # no --dir
        self.store.corrupt = "releases/v26.10.4/AetherSDR-x.update.tar"
        with self.assertRaises(pub.PublishError):
            pub.publish(self.store, "v26.10.4", "v26.10.4", pairs, self.pub, payload_dir=self.payload_dir())
        self.assertFalse([k for k in self.store.objects if "manifest" in k], self.store.objects)
        self.store.corrupt = None
        d = self.payload_dir()
        (d / "AetherSDR-x.update.tar").write_bytes(b"not the bundle the manifest pins")
        with self.assertRaises(pub.PublishError):
            pub.publish(self.store, "v26.10.4", "v26.10.4", pairs, self.pub, payload_dir=d)
        self.assertFalse([k for k in self.store.objects if "manifest" in k])

    def test_a_repair_reuses_the_same_bytes_and_rewrites_other_bytes(self) -> None:
        pairs = self.signed("v26.10.4", bundle_base=self.R2)
        key = "releases/v26.10.4/AetherSDR-x.update.tar"
        self.store.objects[key] = self.BUNDLE           # an earlier run's upload, same bytes
        puts = []
        real_put_file = self.store.put_file
        self.store.put_file = lambda *a, **k: (puts.append(a[0]), real_put_file(*a, **k))
        pub.publish(self.store, "v26.10.4", "v26.10.4", pairs, self.pub, payload_dir=self.payload_dir())
        self.assertNotIn(key, puts, "identical bytes are reused, not uploaded again")
        self.assertTrue(pub.is_complete(self.store, "v26.10.4", "v26.10.4", self.pub))
        other = FakeStore()
        other.objects[key] = b"a different build of the same bundle"   # never wedges the release
        pub.publish(other, "v26.10.4", "v26.10.4", pairs, self.pub, payload_dir=self.payload_dir())
        self.assertEqual(other.objects[key], self.BUNDLE, "rebuilt bytes replace the stored ones")
        self.assertTrue(pub.is_complete(other, "v26.10.4", "v26.10.4", self.pub))

    def test_a_percent_encoded_url_is_refused(self) -> None:
        """No payload URL carries an escape, so the raw prefix tests in
        r2_assets/payload_base can't disagree with the decoded one."""
        for url in (self.R2 + "v26.10.4%2Fx.update.tar", self.R2 + "v26.10.4/a%2Fb.update.tar",
                    self.R2 + "v26.10.4/x%2eupdate.tar", pub.DOWNLOAD_BASE + "v26.10.4/x%20y.tar"):
            m = self.body("v26.10.4", V2, self.R2)
            m["payloads"][0]["patches"][0]["bundle"]["url"] = url
            # The decoded name, so the asset-name check passes and only the
            # escape itself can refuse it.
            m["payloads"][0]["patches"][0]["bundle"]["asset"] = pub.urllib.parse.unquote(url).rsplit("/", 1)[1]
            with self.subTest(url=url), self.assertRaises(pub.PublishError):
                pub.validate_manifest(json.dumps(m).encode(), "v26.10.4", V2)

    def test_r2_assets_and_payload_base_agree_with_validation(self) -> None:
        for base in (pub.DOWNLOAD_BASE, self.R2):
            m = self.body("v26.10.4", V2, base)
            pub.validate_manifest(json.dumps(m).encode(), "v26.10.4", V2)
            urls = [a["url"] for a, _ in pub.payload_assets(m)]
            on_r2 = [u for u in urls if pub.one_file_under(u, self.R2, "v26.10.4")]
            self.assertEqual([a["url"] for a in pub.r2_assets(m, "v26.10.4")], on_r2)
            self.assertEqual(pub.payload_base(m), "r2" if base == self.R2 else "github")

    def test_a_malformed_manifest_is_a_publish_error(self) -> None:
        def shaped(edit):
            m = self.body("v26.10.4", V2)
            edit(m)
            return m
        cases = {
            "no full": lambda m: m["payloads"][0].pop("full"),
            "full not an object": lambda m: m["payloads"][0].__setitem__("full", "x.zip"),
            "full without url": lambda m: m["payloads"][0]["full"].pop("url"),
            "patch without bundle": lambda m: m["payloads"][0]["patches"][0].pop("bundle"),
            "bundle url not a string": lambda m: m["payloads"][0]["patches"][0]["bundle"].__setitem__("url", 7),
            "patches not a list": lambda m: m["payloads"][0].__setitem__("patches", {}),
            "payload not an object": lambda m: m.__setitem__("payloads", ["x"]),
        }
        for label, edit in cases.items():
            m = shaped(edit)
            with self.subTest(label):
                with self.assertRaises(pub.PublishError):
                    pub.validate_manifest(json.dumps(m).encode(), "v26.10.4", V2)
                with self.assertRaises(pub.PublishError):
                    pub.r2_assets(m, "v26.10.4")
                with self.assertRaises(pub.PublishError):
                    pub.payload_base(m)
        with self.assertRaises(pub.PublishError):
            pub.validate_manifest(b"[1, 2]", "v26.10.4", V2)

    def test_a_payload_must_be_served_under_its_own_name(self) -> None:
        m = self.body("v26.10.4", bundle_base=self.R2)
        m["payloads"][0]["patches"][0]["bundle"]["asset"] = "../../elsewhere.tar"
        with self.assertRaises(pub.PublishError):
            pub.validate_manifest(json.dumps(m).encode(), "v26.10.4", V2)

    def test_github_bundles_are_not_uploaded(self) -> None:
        pub.publish(self.store, "v26.10.4", "v26.10.4", self.signed("v26.10.4"), self.pub)
        self.assertFalse([k for k in self.store.objects if k.endswith(".update.tar")])

    def test_both_manifests_must_verify_before_any_write(self) -> None:
        pairs = self.signed("v26.10.4")
        pairs[V2] = (pairs[V2][0], bytes(64))   # manifest-v2's signature does not verify
        with self.assertRaises(pub.PublishError):
            pub.publish(self.store, "v26.10.4", "v26.10.4", pairs, self.pub)
        self.assertEqual(self.store.objects, {})
        with self.assertRaises(pub.PublishError):
            pub.publish(self.store, "v26.10.4", "v26.10.4", {LEGACY: self.signed("v26.10.4")[LEGACY]},
                        self.pub)
        self.assertEqual(self.store.objects, {})

    def test_latest_is_complete_only_with_both_pairs(self) -> None:
        pairs = self.signed("v26.10.4")
        self.store.fail_on = "latest/manifest.json"
        with self.assertRaises(pub.PublishError):
            pub.publish(self.store, "v26.10.4", "v26.10.4", pairs, self.pub)
        self.assertEqual(self.latest_tag(V2), "v26.10.4")
        self.assertFalse(pub.is_complete(self.store, "v26.10.4", "v26.10.4", self.pub),
                         "latest/ with only manifest-v2.json is torn")
        self.store.fail_on = None
        pub.publish(self.store, "v26.10.4", "v26.10.4", pairs, self.pub)
        self.assertTrue(pub.is_complete(self.store, "v26.10.4", "v26.10.4", self.pub))

    def test_a_release_published_before_the_bridge_is_repaired(self) -> None:
        """v26.11.2's state: only the legacy pair exists. It is incomplete,
        and publishing again writes manifest-v2.json beside it."""
        pairs = self.signed("v26.10.4")
        for prefix in ("releases/v26.10.4", "latest"):
            self.store.objects[f"{prefix}/{LEGACY}"] = pairs[LEGACY][0]
            self.store.objects[f"{prefix}/{LEGACY}.sig"] = pairs[LEGACY][1]
        self.assertFalse(pub.is_complete(self.store, "v26.10.4", "v26.10.4", self.pub))
        pub.publish(self.store, "v26.10.4", "v26.10.4", pairs, self.pub)
        self.assertTrue(pub.is_complete(self.store, "v26.10.4", "v26.10.4", self.pub))

    def test_payload_base(self) -> None:
        self.assertEqual(pub.payload_base(self.body("v26.10.4")), "github")
        self.assertEqual(pub.payload_base(self.body("v26.10.4", V2, self.R2)), "r2")
        self.assertEqual(pub.payload_base(self.body("v26.10.4", LEGACY, self.R2)), "none")
        mixed = self.body("v26.10.4", V2, self.R2)
        mixed["payloads"].append(self.body("v26.10.4")["payloads"][0])
        with self.assertRaises(pub.PublishError):
            pub.payload_base(mixed)

    def test_the_cli_validates_both_and_reports_the_base(self) -> None:
        d = self.tmp / "out"
        d.mkdir()
        for name in pub.MANIFESTS:
            (d / name).write_text(json.dumps(self.body("v26.10.4", name, self.R2)))
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertEqual(pub.main(["validate", "--tag", "v26.10.4", "--dir", str(d)]), 0)
            self.assertEqual(pub.main(["payload-base", "--tag", "v26.10.4", "--dir", str(d)]), 0)
        self.assertEqual(out.getvalue().strip().splitlines()[-1], "r2")
        (d / LEGACY).write_text(json.dumps(self.body("v26.10.4", V2)))   # GitHub patches while v2 is R2
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(pub.main(["validate", "--tag", "v26.10.4", "--dir", str(d)]), 1)


class LatestPromotion(Base):
    def test_older_run_resuming_after_a_newer_one_does_not_roll_latest_back(self) -> None:
        """The review's interleaving: the v26.10.4 run saw itself as GitHub's
        latest, paused, and resumes after v26.10.5 has published."""
        pub.publish(self.store, "v26.10.5", "v26.10.5", self.signed("v26.10.5"), self.pub)
        written = pub.publish(self.store, "v26.10.4", "v26.10.4", self.signed("v26.10.4"), self.pub)
        self.assertEqual(written, ["releases/v26.10.4"])
        self.assertEqual(self.latest_tag(), "v26.10.5")
        self.assertTrue(pub.is_complete(self.store, "v26.10.4", "v26.10.4", self.pub),
                        "a newer latest/ completes an older release")

    def test_a_newer_legacy_latest_alone_is_not_rolled_back(self) -> None:
        """v26.11.2's latest/ shape: only a valid newer legacy pair, no v2.
        An older run must leave it, though there is no manifest-v2 to block it."""
        newer = self.signed("v26.10.5")[LEGACY]
        self.store.objects["latest/manifest.json"] = newer[0]
        self.store.objects["latest/manifest.json.sig"] = newer[1]
        written = pub.publish(self.store, "v26.10.4", "v26.10.4", self.signed("v26.10.4"), self.pub)
        self.assertEqual(written, ["releases/v26.10.4"])
        self.assertEqual(self.latest_tag(LEGACY), "v26.10.5")
        self.assertIsNone(self.latest_tag(V2))

    def test_a_torn_latest_is_repaired_when_this_is_githubs_latest(self) -> None:
        """A newer run killed between latest/'s two writes leaves a manifest
        no signature verifies, and that release may since have been pulled.
        Only a verified newer latest/ blocks promotion: a release GitHub calls
        latest repairs a torn pair, so latest/ never stays broken."""
        for name, (newer, _) in self.signed("v26.10.5").items():
            self.store.objects[f"latest/{name}"] = newer   # its .sig never written
        pub.publish(self.store, "v26.10.4", "v26.10.4", self.signed("v26.10.4"), self.pub)
        self.assertEqual(self.latest_tag(), "v26.10.4")
        self.assertTrue(pub.is_complete(self.store, "v26.10.4", "v26.10.4", self.pub))

    def test_hotfix_promotes_over_its_base(self) -> None:
        pub.publish(self.store, "v26.10.4", "v26.10.4", self.signed("v26.10.4"), self.pub)
        pub.publish(self.store, "v26.10.4.1", "v26.10.4.1", self.signed("v26.10.4.1"), self.pub)
        self.assertEqual(self.latest_tag(), "v26.10.4.1")

    def test_not_the_latest_release_leaves_latest_alone(self) -> None:
        pub.publish(self.store, "v26.10.5", "v26.10.5", self.signed("v26.10.5"), self.pub)
        pub.publish(self.store, "v26.10.6", "v26.10.5", self.signed("v26.10.6"), self.pub)
        self.assertEqual(self.latest_tag(), "v26.10.5")

    def test_publish_job_is_serialized_across_tags(self) -> None:
        """The re-read before the write closes the gap only because no other
        tag's publish runs at the same time: the job's concurrency group
        must not be keyed by tag."""
        wf = (REPO / ".github/workflows/update-manifest.yml").read_text()
        job = wf.split("\n  publish:\n", 1)[1]
        m = re.search(r"\n    concurrency:\n      group: ([^\n]+)\n      cancel-in-progress: false\n", job)
        self.assertIsNotNone(m, "the publish job has its own concurrency group")
        self.assertEqual(m.group(1).strip(), "update-manifest-publish")


if __name__ == "__main__":
    unittest.main(verbosity=1)
