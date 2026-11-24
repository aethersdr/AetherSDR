#!/usr/bin/env python3
"""Tests for offload_release_assets.py: what leaves the release page, when,
and that nothing is deleted unless every copy checks out (RFC #6964)."""

from __future__ import annotations

import contextlib
import hashlib
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import offload_release_assets as o  # noqa: E402

TAG = "v26.11.4"
P = f"AetherSDR-{TAG}-"
SIGNED_AT = "2026-11-22T10:40:00Z"
BUILT_AT = "2026-11-22T10:20:00Z"

KEPT = [
    f"{P}x86_64.AppImage", f"{P}aarch64.AppImage", f"{P}x86_64.tar.zst", f"{P}aarch64.tar.zst",
    f"{P}Windows-x64-setup.exe", f"{P}Windows-x64-portable.zip",
    f"{P}macOS-apple-silicon.dmg", f"{P}macOS-intel.dmg",
    f"{P}macOS-apple-silicon.app.tar.zst", f"{P}macOS-intel.app.tar.zst",
    f"{P}source.tar.gz", f"AetherSDR-Manual-{TAG}.pdf", "SHA256SUMS.txt", "SHA256SUMS.txt.asc",
]
# What the signing run signs, written out rather than read from the code
# under test.
SIGNED = [f"{P}x86_64.AppImage", f"{P}aarch64.AppImage", f"{P}x86_64.tar.zst", f"{P}aarch64.tar.zst",
          f"{P}Windows-x64-setup.exe", f"{P}Windows-x64-portable.zip",
          f"{P}macOS-apple-silicon.app.tar.zst", f"{P}macOS-intel.app.tar.zst", f"{P}source.tar.gz"]
MOVED = sorted([n + ".asc" for n in SIGNED] + [
    f"{P}x86_64-symbols.tar.xz", f"{P}aarch64-symbols.tar.xz", f"{P}Windows-x64-symbols.tar.xz",
    f"{P}macOS-apple-silicon-symbols.tar.xz", f"{P}macOS-intel-symbols.tar.xz",
    "AetherSDR-26.11.4.0-Windows-x64.msixupload",
])


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


class FakeGitHub:
    def __init__(self, names: list[str], busy: list[str] | None = None) -> None:
        self.files: dict[str, bytes] = {}
        self.meta: dict[str, dict] = {}
        self._busy = busy or []
        self.deleted: list[str] = []
        self.on_download = None
        for i, n in enumerate(names):
            at = SIGNED_AT if n.startswith("SHA256SUMS") or n.endswith(".asc") else BUILT_AT
            self.attach(n, f"bytes of {n}".encode(), at, 1000 + i)

    def attach(self, name: str, data: bytes, at: str, asset_id: int) -> None:
        self.files[name] = data
        self.meta[name] = {"id": asset_id, "name": name, "size": len(data),
                           "digest": f"sha256:{sha(data)}", "updatedAt": at}

    def releases(self):
        return [{"tag_name": TAG}]

    def fail_delete(self, name):
        self._fail_delete = name

    def assets(self, tag):
        return [dict(m) for m in self.meta.values()]

    def busy(self, tag):
        return list(self._busy)

    def download(self, tag, name, into):
        if self.on_download:
            self.on_download(name)
        path = into / name
        path.write_bytes(self.files[name])
        return path

    def delete(self, asset_id):
        name = next(n for n, m in self.meta.items() if m["id"] == asset_id)
        if getattr(self, "_fail_delete", None) == name:
            raise o.OffloadError("HTTP 404")
        del self.meta[name], self.files[name]
        self.deleted.append(name)


class FakeBucket:
    def __init__(self, fail_on: str | None = None, tamper=None) -> None:
        """tamper: (key suffix, bytes -> bytes), what the bucket stores instead."""
        self.objects: dict[str, bytes] = {}
        self.fail_on, self.tamper = fail_on, tamper

    def put_file(self, key, path, content_type, cache_control):
        if self.fail_on and key.endswith(self.fail_on):
            raise o.PublishError(f"PUT {key}: HTTP 500")
        data = path.read_bytes()
        if self.tamper and key.endswith(self.tamper[0]):
            data = self.tamper[1](data)
        self.objects[key] = data

    def digest(self, key):
        d = self.objects.get(key)
        return None if d is None else (sha(d), len(d))


class FakePublic:
    def __init__(self, bucket: FakeBucket, manifests: dict[str, dict] | None = None,
                 unserved: str | None = None, stale: str | None = None) -> None:
        self.bucket, self.manifests, self.unserved, self.stale = bucket, manifests or {}, unserved, stale
        self.sized: list[str] = []

    def get(self, key):
        m = self.manifests.get(key)
        return None if m is None else json.dumps(m).encode()

    def size(self, key):
        self.sized.append(key)
        key = key.partition("?")[0]
        if self.unserved and key.endswith(self.unserved):
            return None
        d = self.bucket.objects.get(key)
        if d is not None and self.stale and key.endswith(self.stale):
            return len(d) + 1
        return None if d is None else len(d)


def run(gh, bucket=None, public=None):
    bucket = bucket or FakeBucket()
    public = public or FakePublic(bucket)
    with tempfile.TemporaryDirectory() as d:
        return o.offload(gh, bucket, public, TAG, Path(d), log=lambda *_: None), bucket


class MoveSet(unittest.TestCase):
    def test_it_is_the_signatures_symbols_and_msixupload_and_nothing_else(self):
        gh = FakeGitHub(KEPT + MOVED)
        self.assertEqual([a["name"] for a in o.moves(TAG, gh.assets(TAG))], MOVED)

    def test_the_page_keeps_fourteen_files(self):
        gh = FakeGitHub(KEPT + MOVED)
        moved = {a["name"] for a in o.moves(TAG, gh.assets(TAG))}
        # RFC #6964: six installers, the manual, the source, both sums files
        # and the four full update tarballs.
        self.assertEqual(sorted(n for n in gh.meta if n not in moved), sorted(KEPT))
        self.assertEqual(len(KEPT), 14)

    def test_a_hotfix_without_an_msixupload_moves_the_rest(self):
        tag = "v26.11.4.1"
        names = [f"AetherSDR-{tag}-x86_64.AppImage.asc", f"AetherSDR-{tag}-x86_64-symbols.tar.xz"]
        got = o.moves(tag, [{"name": n} for n in names])
        self.assertEqual([a["name"] for a in got], sorted(names))

    def test_another_tags_files_and_the_sums_signature_stay(self):
        names = ["SHA256SUMS.txt.asc", "AetherSDR-v26.11.3-x86_64.AppImage.asc",
                 "AetherSDR-v26.11.3-x86_64-symbols.tar.xz", "AetherSDR-26.11.3.0-Windows-x64.msixupload",
                 "AetherSDR-v26.11.40-x86_64-symbols.tar.xz"]
        self.assertEqual(o.moves(TAG, [{"name": n} for n in names]), [])

    def test_patch_bundles_and_payloads_never_move(self):
        names = [f"{P}x86_64-from-v26.11.3.update.tar", f"{P}x86_64.tar.zst", f"{P}Windows-x64-portable.zip"]
        self.assertEqual(o.moves(TAG, [{"name": n} for n in names]), [])

class Eligibility(unittest.TestCase):
    def test_the_bridge_and_older_releases_are_not_trimmed(self):
        for tag in ("v26.11.3", "v26.11.2", "v26.10.4"):
            self.assertFalse(o.eligible(tag), tag)

    def test_from_the_release_after_the_bridge(self):
        for tag in ("v26.11.4", "v26.11.4.1", "v26.12.1", "offload-test-1"):
            self.assertTrue(o.eligible(tag), tag)

    def test_a_real_run_for_an_older_tag_is_refused_and_a_dry_run_is_not(self):
        gh = FakeGitHub([])
        pub = FakePublic(FakeBucket())
        with self.assertRaises(o.OffloadError):
            o.plan(gh, pub, "v26.11.2", dry_run=False)
        self.assertEqual(o.plan(gh, pub, "v26.11.2", dry_run=True).why, "nothing to move")

    def test_a_tag_that_is_not_a_release_or_test_tag_is_refused(self):
        for tag in ("main", "v26.11.4-rc1", "offload-test-x", "../v26.11.4"):
            with self.assertRaises(o.OffloadError, msg=tag):
                o.plan(FakeGitHub([]), FakePublic(FakeBucket()), tag, dry_run=True)


class Readiness(unittest.TestCase):
    def test_settled_and_signed_is_ready(self):
        gh = FakeGitHub(KEPT + MOVED)
        self.assertTrue(o.plan(gh, FakePublic(FakeBucket()), None, dry_run=False).ready)

    def test_a_run_still_going_holds_it(self):
        gh = FakeGitHub(KEPT + MOVED, busy=["macOS DMG"])
        p = o.plan(gh, FakePublic(FakeBucket()), None, dry_run=False)
        self.assertFalse(p.ready)
        self.assertIn("macOS DMG", p.why)

    def test_unsigned_holds_it(self):
        gh = FakeGitHub([n for n in KEPT + MOVED if not n.startswith("SHA256SUMS")])
        self.assertFalse(o.plan(gh, FakePublic(FakeBucket()), None, dry_run=False).ready)

    def test_a_file_attached_after_signing_holds_it(self):
        gh = FakeGitHub(KEPT + MOVED)
        gh.meta[f"{P}x86_64.AppImage"]["updatedAt"] = "2026-11-22T11:00:00Z"
        p = o.plan(gh, FakePublic(FakeBucket()), None, dry_run=False)
        self.assertFalse(p.ready)
        self.assertIn("x86_64.AppImage", p.why)

    def test_the_source_tarball_uploaded_just_after_the_sums_does_not_hold_it(self):
        # v26.11.2: SHA256SUMS.txt at 02:34:44, the source tarball at 02:34:51,
        # both from the same signing run.
        gh = FakeGitHub(KEPT + MOVED)
        gh.meta[f"{P}source.tar.gz"]["updatedAt"] = "2026-11-22T10:40:07Z"
        self.assertTrue(o.plan(gh, FakePublic(FakeBucket()), None, dry_run=False).ready)

    def test_an_older_release_that_is_ready_is_not_hidden_by_a_newer_one_still_building(self):
        gh = FakeGitHub(KEPT + MOVED)
        newer = "v26.11.5"
        building = FakeGitHub([f"AetherSDR-{newer}-x86_64-symbols.tar.xz"])

        class Two(FakeGitHub):
            def releases(self):
                return [{"tag_name": newer}, {"tag_name": TAG}]

            def assets(self, tag):
                return (building if tag == newer else gh).assets(tag)

            def busy(self, tag):
                return ["AppImage"] if tag == newer else []
        p = o.plan(Two([]), FakePublic(FakeBucket()), None, dry_run=False)
        self.assertEqual((p.tag, p.ready), (TAG, True))

    def test_with_none_ready_the_newest_with_files_says_why(self):
        newer = "v26.11.5"

        class Two(FakeGitHub):
            def releases(self):
                return [{"tag_name": TAG}, {"tag_name": newer}]

            def busy(self, tag):
                return ["AppImage"]
        g = Two([])
        g.attach(f"AetherSDR-{newer}-x86_64-symbols.tar.xz", b"s", BUILT_AT, 1)
        g.attach(f"{P}x86_64-symbols.tar.xz", b"s", BUILT_AT, 2)
        p = o.plan(g, FakePublic(FakeBucket()), None, dry_run=False)
        self.assertEqual(p.tag, newer)
        self.assertFalse(p.ready)

    def test_a_plan_makes_no_requests_to_updates_aethersdr_com(self):
        bucket = FakeBucket()
        pub = FakePublic(bucket)
        o.plan(FakeGitHub(KEPT + MOVED), pub, None, dry_run=False)
        self.assertEqual(pub.sized, [])

    def test_after_the_move_there_is_nothing_to_plan(self):
        gh = FakeGitHub(KEPT)
        p = o.plan(gh, FakePublic(FakeBucket()), None, dry_run=False)
        self.assertIsNone(p.tag)
        self.assertFalse(p.ready)

    def test_a_published_manifest_naming_a_moved_file_is_refused(self):
        name = f"{P}Windows-x64-symbols.tar.xz"
        m = {"payloads": [{"full": {"url": f"{o.DOWNLOAD_BASE}{TAG}/{name}"}, "patches": []}]}
        bucket = FakeBucket()
        pub = FakePublic(bucket, {f"releases/{TAG}/manifest.json": m})
        gh = FakeGitHub(KEPT + MOVED)
        with self.assertRaises(o.OffloadError):
            run(gh, bucket, pub)
        self.assertEqual(bucket.objects, {})
        self.assertEqual(gh.deleted, [])


class Offload(unittest.TestCase):
    def test_copies_checks_then_deletes_every_moved_file(self):
        gh = FakeGitHub(KEPT + MOVED)
        before = dict(gh.files)
        moved, bucket = run(gh)
        self.assertEqual(moved, MOVED)
        self.assertEqual(sorted(gh.deleted), MOVED)
        self.assertEqual(sorted(gh.meta), sorted(KEPT))
        for n in MOVED:
            self.assertEqual(bucket.objects[f"releases/{TAG}/{n}"], before[n])

    def test_a_failed_upload_deletes_nothing(self):
        gh = FakeGitHub(KEPT + MOVED)
        with self.assertRaises(o.PublishError):
            run(gh, FakeBucket(fail_on="Windows-x64-symbols.tar.xz"))
        self.assertEqual(gh.deleted, [])

    def test_a_copy_that_reads_back_wrong_deletes_nothing(self):
        gh = FakeGitHub(KEPT + MOVED)
        with self.assertRaises(o.OffloadError):
            run(gh, FakeBucket(tamper=("msixupload", lambda d: d[:-1])))
        self.assertEqual(gh.deleted, [])

    def test_a_copy_of_the_same_length_with_other_bytes_deletes_nothing(self):
        # Only the bucket's SHA-256 tells these apart: size and served size match.
        gh = FakeGitHub(KEPT + MOVED)
        name = f"{P}Windows-x64-symbols.tar.xz"
        good = gh.files[name]
        bad = good[:-1] + bytes([good[-1] ^ 0x01])
        bucket = FakeBucket(tamper=(name, lambda d: bad))
        with self.assertRaises(o.OffloadError) as e:
            run(gh, bucket)
        self.assertEqual(len(bad), len(good))
        self.assertEqual(str(e.exception), f"releases/{TAG}/{name}: the bucket holds "
                         f"{(sha(bad), len(bad))}, not {(sha(good), len(good))}")
        self.assertEqual(gh.deleted, [])

    def test_a_copy_not_served_publicly_deletes_nothing(self):
        gh = FakeGitHub(KEPT + MOVED)
        bucket = FakeBucket()
        with self.assertRaises(o.OffloadError):
            run(gh, bucket, FakePublic(bucket, unserved="source.tar.gz.asc"))
        self.assertEqual(gh.deleted, [])

    def test_a_stale_size_served_deletes_nothing(self):
        gh = FakeGitHub(KEPT + MOVED)
        bucket = FakeBucket()
        with self.assertRaises(o.OffloadError):
            run(gh, bucket, FakePublic(bucket, stale="x86_64.AppImage.asc"))
        self.assertEqual(gh.deleted, [])

    def test_the_served_check_asks_past_the_edge_cache(self):
        gh = FakeGitHub(KEPT + MOVED)
        bucket = FakeBucket()
        pub = FakePublic(bucket)
        run(gh, bucket, pub)
        self.assertTrue(pub.sized)
        self.assertTrue(all("?offload-check=" in k for k in pub.sized))

    def test_a_signing_run_started_during_the_copy_deletes_nothing(self):
        gh = FakeGitHub(KEPT + MOVED)

        def start(downloading):
            if downloading == MOVED[-1]:
                gh._busy = [o.SIGN_WORKFLOW]
        gh.on_download = start
        with self.assertRaises(o.OffloadError):
            run(gh)
        self.assertEqual(gh.deleted, [])

    def test_a_failed_delete_says_a_rerun_finishes_and_the_rerun_does(self):
        gh = FakeGitHub(KEPT + MOVED)
        gh.fail_delete(MOVED[3])
        bucket = FakeBucket()
        with self.assertRaises(o.OffloadError) as e:
            run(gh, bucket)
        self.assertIn("run again", str(e.exception))
        gh.fail_delete(None)
        moved, _ = run(gh, bucket)
        self.assertIn(MOVED[3], moved)
        self.assertEqual(sorted(gh.meta), sorted(KEPT))

    def test_a_queued_second_run_with_nothing_left_succeeds(self):
        gh = FakeGitHub(KEPT + MOVED)
        _, bucket = run(gh)
        moved, _ = run(gh, bucket)
        self.assertEqual(moved, [])

    def test_a_download_that_differs_from_githubs_digest_deletes_nothing(self):
        gh = FakeGitHub(KEPT + MOVED)
        name = f"{P}x86_64.AppImage.asc"
        gh.files[name] = b"x" * gh.meta[name]["size"]
        with self.assertRaises(o.OffloadError):
            run(gh)
        self.assertEqual(gh.deleted, [])

    def test_a_signature_replaced_during_the_copy_deletes_nothing(self):
        gh = FakeGitHub(KEPT + MOVED)
        name = f"{P}x86_64.AppImage.asc"

        def resign(downloading):
            if downloading == MOVED[-1]:
                gh.attach(name, b"a newer signature", SIGNED_AT, 9999)
        gh.on_download = resign
        with self.assertRaises(o.OffloadError):
            run(gh)
        self.assertEqual(gh.deleted, [])

    def test_a_tag_not_ready_is_refused_before_anything_is_copied(self):
        gh = FakeGitHub(KEPT + MOVED, busy=[o.SIGN_WORKFLOW])
        bucket = FakeBucket()
        with self.assertRaises(o.OffloadError):
            run(gh, bucket)
        self.assertEqual(bucket.objects, {})

    def test_a_second_run_after_a_resign_moves_the_new_signatures(self):
        gh = FakeGitHub(KEPT + MOVED)
        _, bucket = run(gh)
        name = f"{P}x86_64.AppImage.asc"
        gh.attach(name, b"re-signed", SIGNED_AT, 5000)
        moved, _ = run(gh, bucket)
        self.assertEqual(moved, [name])
        self.assertEqual(bucket.objects[f"releases/{TAG}/{name}"], b"re-signed")


class BusyQueries(unittest.TestCase):
    """GitHub.busy, against a stand-in for the gh CLI."""

    def busy(self, unfinished_sign_status=None, build_status="completed"):
        calls = []

        def fake(self_, *args):
            calls.append(args)
            if o.SIGN_WORKFLOW in args:
                status = args[args.index("--status") + 1]
                return json.dumps([{"status": status}] if status == unfinished_sign_status else [])
            return json.dumps([{"status": build_status}])
        gh = o.GitHub("aethersdr/AetherSDR")
        orig = o.GitHub._gh
        o.GitHub._gh = fake
        try:
            return gh.busy(TAG), calls
        finally:
            o.GitHub._gh = orig

    def test_a_signing_run_waiting_for_approval_counts_however_many_runs_are_newer(self):
        busy, calls = self.busy(unfinished_sign_status="waiting")
        self.assertEqual(len(busy), 1)
        self.assertIn(o.SIGN_WORKFLOW, busy[0])
        sign = [c for c in calls if o.SIGN_WORKFLOW in c]
        self.assertTrue(all("--status" in c for c in sign))

    def test_every_unfinished_state_is_asked_about(self):
        for status in o.UNFINISHED:
            self.assertTrue(self.busy(unfinished_sign_status=status)[0], status)

    def test_a_build_still_running_for_the_tag_counts(self):
        busy, calls = self.busy(build_status="in_progress")
        self.assertIn("AppImage", busy)
        self.assertIn("Docs", busy)
        builds = [c for c in calls if "AppImage" in c]
        self.assertIn(TAG, builds[0])

    def test_all_finished_is_not_busy(self):
        self.assertEqual(self.busy()[0], [])


class Cli(unittest.TestCase):
    def plan_cli(self, *args):
        gh = FakeGitHub(KEPT + MOVED)
        pub = FakePublic(FakeBucket())
        orig = o.GitHub, o.PublicStore, o.os.environ.get("GH_REPO")
        o.GitHub, o.PublicStore = (lambda repo: gh), (lambda: pub)
        o.os.environ["GH_REPO"] = "aethersdr/AetherSDR"
        out = io.StringIO()
        try:
            with tempfile.TemporaryDirectory() as d, contextlib.redirect_stdout(out):
                ghout = Path(d) / "out"
                o.main(["plan", *args, "--github-output", str(ghout)])
                written = ghout.read_text()
        finally:
            o.GitHub, o.PublicStore = orig[0], orig[1]
            if orig[2] is None:
                del o.os.environ["GH_REPO"]
            else:
                o.os.environ["GH_REPO"] = orig[2]
        return pub, written, out.getvalue()

    def test_a_plan_on_a_signing_run_asks_nothing_of_updates_aethersdr_com(self):
        pub, written, _ = self.plan_cli()
        self.assertEqual(pub.sized, [])
        self.assertIn(f"tag={TAG}\nready=true\n", written)

    def test_a_dry_run_says_what_is_on_r2_and_is_never_ready(self):
        pub, written, out = self.plan_cli("--tag", TAG, "--dry-run")
        self.assertEqual(len(pub.sized), len(MOVED))
        self.assertIn("not on R2 yet", out)
        self.assertIn("ready=false", written)


class ContentTypes(unittest.TestCase):
    def test_each_kind(self):
        self.assertEqual(o.content_type("a.AppImage.asc"), "application/pgp-signature")
        self.assertEqual(o.content_type("a-symbols.tar.xz"), "application/x-xz")
        self.assertEqual(o.content_type("a.msixupload"), "application/octet-stream")


if __name__ == "__main__":
    unittest.main()
