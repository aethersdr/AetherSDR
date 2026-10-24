#!/usr/bin/env python3
"""Tests for tools/publish_update_manifest.py, against an in-memory store
and a real (ephemeral) Ed25519 key. No network, no R2 credentials."""

from __future__ import annotations

import json
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import publish_update_manifest as pub  # noqa: E402

REPO = Path(__file__).resolve().parent.parent


class FakeStore:
    def __init__(self) -> None:
        self.objects: dict[str, bytes] = {}
        self.fail_on: str | None = None   # a key whose PUT fails

    def get(self, key: str) -> bytes | None:
        return self.objects.get(key)

    def put(self, key: str, data: bytes, content_type: str, cache_control: str) -> None:
        if key == self.fail_on:
            raise pub.PublishError(f"PUT {key}: HTTP 500")
        self.objects[key] = data


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

    @staticmethod
    def body(release: str, **override) -> dict:
        base = pub.DOWNLOAD_BASE + release + "/"
        m = {"schema": 1, "tag": release, "version": release[1:], "key_id": "788fb6e31f7ecba4",
             "payloads": [{"platform": "windows", "arch": "x86_64",
                           "full": {"url": base + "AetherSDR-x.zip"},
                           "patches": [{"bundle": {"url": base + "AetherSDR-x.update.tar"}}]}]}
        m.update(override)
        return m

    def signed(self, tag: str, key: Path | None = None) -> tuple[bytes, bytes]:
        manifest = json.dumps(self.body(tag)).encode()
        m = self.tmp / "m.json"
        m.write_bytes(manifest)
        sig = self.tmp / "m.sig"
        subprocess.run(["openssl", "pkeyutl", "-sign", "-rawin", "-inkey", str(key or self.key),
                        "-in", str(m), "-out", str(sig)], check=True)
        return manifest, sig.read_bytes()

    def latest_tag(self) -> str | None:
        raw = self.store.objects.get("latest/manifest.json")
        return json.loads(raw)["tag"] if raw else None


class Completeness(Base):
    def test_publish_then_complete(self) -> None:
        m, s = self.signed("v26.10.4")
        self.assertFalse(pub.is_complete(self.store, "v26.10.4", "v26.10.4", self.pub))
        self.assertEqual(pub.publish(self.store, "v26.10.4", "v26.10.4", m, s, self.pub),
                         ["releases/v26.10.4", "latest"])
        self.assertTrue(pub.is_complete(self.store, "v26.10.4", "v26.10.4", self.pub))

    def test_manifest_without_signature_is_incomplete_and_repaired(self) -> None:
        """The partial publish the review reproduced: the manifest PUT
        succeeded, the signature PUT failed."""
        m, s = self.signed("v26.10.4")
        self.store.fail_on = "releases/v26.10.4/manifest.json.sig"
        with self.assertRaises(pub.PublishError):
            pub.publish(self.store, "v26.10.4", "v26.10.4", m, s, self.pub)
        self.assertIn("releases/v26.10.4/manifest.json", self.store.objects)
        self.assertFalse(pub.is_complete(self.store, "v26.10.4", "v26.10.4", self.pub),
                         "a served manifest with no signature is not a publication")
        self.store.fail_on = None
        pub.publish(self.store, "v26.10.4", "v26.10.4", m, s, self.pub)
        self.assertTrue(pub.is_complete(self.store, "v26.10.4", "v26.10.4", self.pub))

    def test_release_without_latest_is_incomplete_when_it_is_latest(self) -> None:
        m, s = self.signed("v26.10.4")
        self.store.fail_on = "latest/manifest.json"
        with self.assertRaises(pub.PublishError):
            pub.publish(self.store, "v26.10.4", "v26.10.4", m, s, self.pub)
        self.assertFalse(pub.is_complete(self.store, "v26.10.4", "v26.10.4", self.pub))
        self.assertTrue(pub.is_complete(self.store, "v26.10.4", "v26.10.5", self.pub),
                        "an older release needs only its own pair")
        self.store.fail_on = None
        pub.publish(self.store, "v26.10.4", "v26.10.4", m, s, self.pub)
        self.assertTrue(pub.is_complete(self.store, "v26.10.4", "v26.10.4", self.pub))

    def test_a_signature_that_does_not_verify_is_incomplete(self) -> None:
        m, s = self.signed("v26.10.4")
        self.store.objects["releases/v26.10.4/manifest.json"] = m
        self.store.objects["releases/v26.10.4/manifest.json.sig"] = bytes(64)
        self.assertFalse(pub.is_complete(self.store, "v26.10.4", "v26.10.5", self.pub))

    def test_wrong_key_is_refused_before_any_write(self) -> None:
        other = self.tmp / "other.pem"
        subprocess.run(["openssl", "genpkey", "-algorithm", "ed25519", "-out", str(other)], check=True)
        m, s = self.signed("v26.10.4", key=other)
        with self.assertRaises(pub.PublishError):
            pub.publish(self.store, "v26.10.4", "v26.10.4", m, s, self.pub)
        self.assertEqual(self.store.objects, {})


class Validation(Base):
    def test_only_a_manifest_for_this_release_may_be_signed(self) -> None:
        pub.validate_manifest(json.dumps(self.body("v26.10.4")).encode(), "v26.10.4")
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
            with self.assertRaises(pub.PublishError):
                pub.validate_manifest(json.dumps(m).encode(), "v26.10.4")
        with self.assertRaises(pub.PublishError):
            pub.validate_manifest(b"not json", "v26.10.4")


class LatestPromotion(Base):
    def test_older_run_resuming_after_a_newer_one_does_not_roll_latest_back(self) -> None:
        """The review's interleaving: the v26.10.4 run saw itself as GitHub's
        latest, paused, and resumes after v26.10.5 has published."""
        new_m, new_s = self.signed("v26.10.5")
        old_m, old_s = self.signed("v26.10.4")
        pub.publish(self.store, "v26.10.5", "v26.10.5", new_m, new_s, self.pub)
        written = pub.publish(self.store, "v26.10.4", "v26.10.4", old_m, old_s, self.pub)
        self.assertEqual(written, ["releases/v26.10.4"])
        self.assertEqual(self.latest_tag(), "v26.10.5")
        self.assertTrue(pub.is_complete(self.store, "v26.10.4", "v26.10.4", self.pub),
                        "a newer latest/ completes an older release")

    def test_hotfix_promotes_over_its_base(self) -> None:
        pub.publish(self.store, "v26.10.4", "v26.10.4", *self.signed("v26.10.4"), self.pub)
        pub.publish(self.store, "v26.10.4.1", "v26.10.4.1", *self.signed("v26.10.4.1"), self.pub)
        self.assertEqual(self.latest_tag(), "v26.10.4.1")

    def test_not_the_latest_release_leaves_latest_alone(self) -> None:
        pub.publish(self.store, "v26.10.5", "v26.10.5", *self.signed("v26.10.5"), self.pub)
        pub.publish(self.store, "v26.10.6", "v26.10.5", *self.signed("v26.10.6"), self.pub)
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
