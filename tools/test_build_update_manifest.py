#!/usr/bin/env python3
"""Tests for tools/build_update_manifest.py.

The round trip is the one that matters: a reference applier, written here from
the schema alone, turns the old tree plus a bundle into a tree whose every
file matches the manifest. The tool's own code is not used to apply, so a
manifest the app could not apply fails here.

Needs the zstd CLI (installed on the GitHub-hosted Ubuntu runners). No
network, no unsquashfs: payloads are zips and synthetic ELF headers.
"""

from __future__ import annotations

import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import tarfile
import tempfile
import unittest
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build_update_manifest as bum  # noqa: E402


def write(path: Path, data: bytes, executable: bool = False) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    path.chmod(0o755 if executable else 0o644)


def apply_patch_set(old_root: Path, bundle: Path, patch: dict, files: list[dict],
                    dest: Path) -> None:
    """A reference applier, from the schema: copy the old tree, apply the
    removals, prune directories they left empty, apply the rest in order,
    then check every listed file. Raises on any mismatch."""
    shutil.copytree(old_root, dest, symlinks=True)
    members = Path(tempfile.mkdtemp())
    with tarfile.open(bundle) as tar:
        tar.extractall(members, filter="data")
    emptied: set[Path] = set()
    for op in patch["ops"]:
        target = dest / op["path"]
        if op["op"] == "remove":
            target.unlink()
            emptied.update(target.parents)
            continue
        if emptied:
            for d in sorted((d for d in emptied if d != dest and dest in d.parents),
                            key=lambda d: len(d.parts), reverse=True):
                if d.is_dir() and not d.is_symlink() and not any(d.iterdir()):
                    d.rmdir()
            emptied.clear()
        if op["op"] == "symlink":
            if target.is_symlink() or target.exists():
                target.unlink()
            target.parent.mkdir(parents=True, exist_ok=True)
            os.symlink(op["target"], target)
            continue
        out = members / (op["member"] + ".out")
        cmd = ["zstd", "-q", "-d", "-f", f"--long={op['window_log']}"]
        if op["op"] == "patch":
            assert bum.sha256_file(target) == op["from_sha256"], "source hash checked first"
            cmd.append(f"--patch-from={target}")
        subprocess.run(cmd + [str(members / op["member"]), "-o", str(out)], check=True)
        assert bum.sha256_file(out) == op["sha256"]
        target.parent.mkdir(parents=True, exist_ok=True)
        out.replace(target)
        target.chmod(0o755 if op["executable"] else 0o644)
    for f in files:
        p = dest / f["path"]
        if f["type"] == "symlink":
            assert p.is_symlink() and os.readlink(p) == f["target"], f["path"]
        else:
            assert bum.sha256_file(p) == f["sha256"], f["path"]
            assert bool(p.stat().st_mode & 0o111) == f["executable"], f["path"]
    listed = {f["path"] for f in files}
    present = {p.relative_to(dest).as_posix() for p in dest.rglob("*")
               if p.is_symlink() or p.is_file()}
    assert present == listed, (present ^ listed)


class Base(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="bum-test-"))

    def tearDown(self) -> None:
        shutil.rmtree(self.tmp, ignore_errors=True)


class WindowLog(Base):
    def test_covers_size(self) -> None:
        self.assertEqual(bum.window_log_for(0), bum.MIN_WINDOW_LOG)
        self.assertEqual(bum.window_log_for(1 << 27), 27)
        self.assertEqual(bum.window_log_for((1 << 27) + 1), 28)
        # The real AetherSDR binary is ~148 MB: a fixed 27 would not cover it.
        self.assertEqual(bum.window_log_for(148_347_993), 28)
        self.assertEqual(bum.window_log_for(1 << 40), bum.MAX_WINDOW_LOG)


class ScanTree(Base):
    def test_files_modes_and_symlinks(self) -> None:
        write(self.tmp / "usr/bin/app", b"binary", executable=True)
        write(self.tmp / "usr/share/a.txt", b"text")
        os.symlink("usr/bin/app", self.tmp / "AppRun")
        tree = bum.scan_tree(self.tmp)
        self.assertEqual(set(tree), {"usr/bin/app", "usr/share/a.txt", "AppRun"})
        self.assertTrue(tree["usr/bin/app"].executable)
        self.assertFalse(tree["usr/share/a.txt"].executable)
        self.assertEqual(tree["AppRun"].type, "symlink")
        self.assertEqual(tree["AppRun"].target, "usr/bin/app")
        self.assertEqual(tree["usr/share/a.txt"].sha256, hashlib.sha256(b"text").hexdigest())


class PatchSet(Base):
    def trees(self) -> tuple[Path, Path]:
        old, new = self.tmp / "old", self.tmp / "new"
        big = os.urandom(200_000)
        write(old / "app.exe", big, executable=True)
        write(new / "app.exe", big[:100_000] + b"CHANGED" + big[100_000:], executable=True)
        write(old / "same.dll", b"same" * 1000)
        write(new / "same.dll", b"same" * 1000)
        write(old / "gone.dll", b"removed")
        write(new / "plugins/new.dll", os.urandom(5000))
        write(old / "mode.sh", b"#!/bin/sh\n")
        write(new / "mode.sh", b"#!/bin/sh\n", executable=True)
        os.symlink("app.exe", old / "link")
        os.symlink("plugins/new.dll", new / "link")
        return old, new

    def test_round_trip(self) -> None:
        old, new = self.trees()
        new_tree = bum.scan_tree(new)
        bundle = self.tmp / "b.update.tar"
        patch = bum.build_patch_set(old, bum.scan_tree(old), new, new_tree, bundle,
                                    self.tmp / "work")
        ops = {o["path"]: o for o in patch["ops"]}
        self.assertEqual(ops["app.exe"]["op"], "patch")
        self.assertLess(bundle.stat().st_size, 60_000, "a small edit ships as a small delta")
        self.assertEqual(ops["plugins/new.dll"]["op"], "add")
        self.assertEqual(ops["gone.dll"]["op"], "remove")
        self.assertEqual(ops["link"], {"op": "symlink", "path": "link", "target": "plugins/new.dll"})
        self.assertIn("mode.sh", ops, "a changed executable bit is an op")
        self.assertNotIn("same.dll", ops)
        self.assertEqual(patch["unchanged"], 1)
        apply_patch_set(old, bundle, patch, bum.files_list(new_tree), self.tmp / "applied")

    def test_identical_trees_need_no_bundle(self) -> None:
        old, new = self.tmp / "old", self.tmp / "new"
        write(old / "a", b"a")
        write(new / "a", b"a")
        bundle = self.tmp / "b.update.tar"
        self.assertIsNone(bum.build_patch_set(old, bum.scan_tree(old), new, bum.scan_tree(new),
                                              bundle, self.tmp / "work"))
        self.assertFalse(bundle.exists())

    def test_bundle_is_deterministic(self) -> None:
        old, new = self.trees()
        sums = []
        for i in range(2):
            bundle = self.tmp / f"b{i}.update.tar"
            bum.build_patch_set(old, bum.scan_tree(old), new, bum.scan_tree(new), bundle,
                                self.tmp / f"work{i}")
            sums.append(bum.sha256_file(bundle))
        self.assertEqual(sums[0], sums[1])


class Symlinks(Base):
    def test_only_links_that_stay_inside_are_accepted(self) -> None:
        ok = [("AppRun", "usr/bin/AetherSDR"), ("usr/lib/libQt6Core.so.6", "libQt6Core.so.6.12.0"),
              ("usr/lib/a/b", "../c"), ("x/y", "./z")]
        bad = [("AppRun", "/usr/bin/AetherSDR"), ("usr/lib/a", "../../../etc"), ("a", ".."),
               ("a", ""), ("a", "..\\..\\x")]
        for rel, target in ok:
            self.assertTrue(bum.symlink_stays_inside(rel, target), (rel, target))
        for rel, target in bad:
            self.assertFalse(bum.symlink_stays_inside(rel, target), (rel, target))

    def test_scan_refuses_an_escaping_link(self) -> None:
        write(self.tmp / "a.txt", b"a")
        os.symlink("/etc/passwd", self.tmp / "evil")
        with self.assertRaises(ValueError):
            bum.scan_tree(self.tmp)


class BuildTimeVerification(Base):
    def test_a_corrupted_bundle_fails_verification(self) -> None:
        old, new = self.tmp / "old", self.tmp / "new"
        big = os.urandom(50_000)
        write(old / "app.exe", big)
        write(new / "app.exe", big[:25_000] + b"X" + big[25_000:])
        new_tree = bum.scan_tree(new)
        bundle = self.tmp / "b.update.tar"
        patch = bum.build_patch_set(old, bum.scan_tree(old), new, new_tree, bundle, self.tmp / "w")
        files = bum.files_list(new_tree)
        bum.verify_patch_set(old, bundle, patch, files, self.tmp / "v1")   # the good bundle passes
        patch["ops"][0]["sha256"] = "0" * 64
        with self.assertRaises(ValueError):
            bum.verify_patch_set(old, bundle, patch, files, self.tmp / "v2")
        write(old / "app.exe", big + b"user-modified")   # the install drifted
        patch["ops"][0]["sha256"] = new_tree["app.exe"].sha256
        with self.assertRaises(ValueError):
            bum.verify_patch_set(old, bundle, patch, files, self.tmp / "v3")


class TypeTransitions(Base):
    """A path that changes kind between releases: removals clear it first."""

    def round_trip(self, setup_old, setup_new) -> None:
        old, new = self.tmp / "old", self.tmp / "new"
        old.mkdir()
        new.mkdir()
        write(old / "keep.txt", b"k")
        write(new / "keep.txt", b"k")
        setup_old(old)
        setup_new(new)
        new_tree = bum.scan_tree(new)
        bundle = self.tmp / "b.update.tar"
        patch = bum.build_patch_set(old, bum.scan_tree(old), new, new_tree, bundle, self.tmp / "w")
        kinds = [o["op"] for o in patch["ops"]]
        self.assertEqual(kinds[: kinds.count("remove")], ["remove"] * kinds.count("remove"),
                         "every removal comes before any write")
        apply_patch_set(old, bundle, patch, bum.files_list(new_tree), self.tmp / "applied")
        bum.verify_patch_set(old, bundle, patch, bum.files_list(new_tree), self.tmp / "verify")

    def test_file_becomes_directory(self) -> None:
        self.round_trip(lambda r: write(r / "plugins", b"was a file"),
                        lambda r: write(r / "plugins/new.dll", b"now a dir"))

    def test_directory_becomes_file(self) -> None:
        self.round_trip(lambda r: write(r / "plugins/old.dll", b"was a dir"),
                        lambda r: write(r / "plugins", b"now a file"))

    def test_nested_directory_becomes_file(self) -> None:
        self.round_trip(lambda r: write(r / "plugins/sub/old.dll", b"was a nested dir"),
                        lambda r: write(r / "plugins", b"now a file"))

    def test_directory_becomes_symlink(self) -> None:
        def new(r: Path) -> None:
            write(r / "real/x.dll", b"x")
            os.symlink("real", r / "plugins")
        self.round_trip(lambda r: write(r / "plugins/old.dll", b"was a dir"), new)


class Versions(Base):
    def test_hotfix_tags_order_and_parse(self) -> None:
        self.assertEqual(bum.version_key("v26.10.3"), (26, 10, 3, 0))
        self.assertEqual(bum.version_key("v26.7.4.1"), (26, 7, 4, 1))
        self.assertLess(bum.version_key("v26.7.4"), bum.version_key("v26.7.4.1"))
        self.assertLess(bum.version_key("v26.7.4.1"), bum.version_key("v26.7.5"))
        for bad in ("v26.10", "v26.10.3-rc1", "lynovation-test-2026-10-16", "v26.10.3.1.2"):
            with self.assertRaises(ValueError):
                bum.version_key(bad)

    def test_earlier_releases_include_hotfixes(self) -> None:
        listing = [
            {"tag": "v26.10.3.1", "draft": False, "prerelease": False},
            {"tag": "v26.10.3", "draft": False, "prerelease": False},
            {"tag": "lynovation-test-2026-10-16", "draft": False, "prerelease": True},
            {"tag": "v26.10.2", "draft": False, "prerelease": False},
            {"tag": "v26.10.1", "draft": True, "prerelease": False},
            {"tag": "v26.7.4.1", "draft": False, "prerelease": False},
            {"tag": "v26.7.4", "draft": False, "prerelease": False},
            {"tag": "v26.11.1", "draft": False, "prerelease": False},
        ]
        real = bum.gh
        bum.gh = lambda *a: "\n".join(json.dumps(r) for r in listing) + "\n"
        try:
            self.assertEqual(bum.earlier_releases("v26.10.4", 4),
                             ["v26.10.3.1", "v26.10.3", "v26.10.2", "v26.7.4.1"])
            self.assertEqual(bum.earlier_releases("v26.10.3.1", 2), ["v26.10.3", "v26.10.2"])
        finally:
            bum.gh = real


class RequiredPayloads(Base):
    def test_a_failed_download_fails_the_release_build(self) -> None:
        """CI mode: a payload that cannot be fetched stops the build; no
        manifest is written for the release with a platform missing."""
        win = self.tmp / "src"
        write(win / "AetherSDR.exe", b"x" * 100)
        good = self.tmp / "AetherSDR-v26.10.3-Windows-x64-portable.zip"
        with zipfile.ZipFile(good, "w") as z:
            z.write(win / "AetherSDR.exe", "AetherSDR.exe")
        real_fetch, real_earlier = bum.fetch_asset, bum.earlier_releases
        bum.fetch_asset = lambda t, n, d, attempts=3: good if n == good.name else None
        bum.earlier_releases = lambda t, depth: []
        try:
            out = self.tmp / "out"
            rc = bum.main(["--tag", "v26.10.3", "--out", str(out), "--fetch-from-github",
                           "--work", str(self.tmp / "w")])
        finally:
            bum.fetch_asset, bum.earlier_releases = real_fetch, real_earlier
        self.assertEqual(rc, 1)
        self.assertFalse((out / "manifest.json").exists())

    def test_local_partial_is_still_allowed(self) -> None:
        d = self.tmp / "dl"
        write(d / "x", b"")
        with zipfile.ZipFile(d / "AetherSDR-v26.10.3-Windows-x64-portable.zip", "w") as z:
            z.writestr("a.dll", b"a")
        self.assertEqual(bum.main(["--tag", "v26.10.3", "--out", str(self.tmp / "o"),
                                   "--assets", f"v26.10.3={d}", "--work", str(self.tmp / "w")]), 0)
        self.assertEqual(bum.main(["--tag", "v26.10.3", "--out", str(self.tmp / "o2"), "--require-all",
                                   "--assets", f"v26.10.3={d}", "--work", str(self.tmp / "w2")]), 1)


class Containers(Base):
    def test_zip_traversal_refused(self) -> None:
        bad = self.tmp / "bad.zip"
        with zipfile.ZipFile(bad, "w") as z:
            z.writestr("../evil.dll", b"x")
        with self.assertRaises(ValueError):
            bum.extract_payload(bad, "zip", self.tmp / "x")

    def test_squashfs_offset_from_elf64_header(self) -> None:
        shoff, shentsize, shnum = 0x400, 0x40, 5
        header = bytearray(0x40)
        header[0:4] = b"\x7fELF"
        header[4] = 2   # 64-bit
        header[5] = 1   # little-endian
        struct.pack_into("<Q", header, 0x28, shoff)
        struct.pack_into("<HH", header, 0x3A, shentsize, shnum)
        image = bytes(header) + b"\0" * (shoff + shentsize * shnum - len(header)) + b"hsqs" + b"\0" * 64
        path = self.tmp / "x.AppImage"
        path.write_bytes(image)
        self.assertEqual(bum.appimage_squashfs_offset(path), shoff + shentsize * shnum)

    def test_squashfs_offset_refuses_a_non_appimage(self) -> None:
        path = self.tmp / "x.AppImage"
        path.write_bytes(b"\x7fELF" + b"\x02\x01" + b"\0" * 200)
        with self.assertRaises(ValueError):
            bum.appimage_squashfs_offset(path)


class Manifest(Base):
    def zip_tree(self, root: Path, out: Path) -> None:
        with zipfile.ZipFile(out, "w") as z:
            for p in sorted(root.rglob("*")):
                if p.is_file():
                    z.write(p, p.relative_to(root).as_posix())

    def test_end_to_end_from_local_assets(self) -> None:
        old, new = self.tmp / "t-old", self.tmp / "t-new"
        write(old / "AetherSDR.exe", b"v1" * 5000)
        write(new / "AetherSDR.exe", b"v2" * 5000)
        write(old / "Qt6Core.dll", b"qt" * 5000)
        write(new / "Qt6Core.dll", b"qt" * 5000)
        dirs = {"v26.10.3": self.tmp / "dl3", "v26.10.2": self.tmp / "dl2"}
        for d in dirs.values():
            d.mkdir()
        self.zip_tree(new, dirs["v26.10.3"] / "AetherSDR-v26.10.3-Windows-x64-portable.zip")
        self.zip_tree(old, dirs["v26.10.2"] / "AetherSDR-v26.10.2-Windows-x64-portable.zip")

        pub = self.tmp / "update-signing.pub"
        subprocess.run(["openssl", "genpkey", "-algorithm", "ed25519", "-out", str(self.tmp / "k")],
                       check=True)
        subprocess.run(["openssl", "pkey", "-in", str(self.tmp / "k"), "-pubout", "-out", str(pub)],
                       check=True)

        out = self.tmp / "out"
        rc = bum.main(["--tag", "v26.10.3", "--out", str(out), "--work", str(self.tmp / "w"),
                       "--public-key", str(pub),
                       "--assets", f"v26.10.3={dirs['v26.10.3']}",
                       "--assets", f"v26.10.2={dirs['v26.10.2']}"])
        self.assertEqual(rc, 0)
        m = json.loads((out / "manifest.json").read_text())
        self.assertEqual(m["schema"], 1)
        self.assertEqual(m["version"], "26.10.3")
        self.assertRegex(m["key_id"], r"^[0-9a-f]{16}$")
        self.assertRegex(m["generated_at"], r"^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ$")
        self.assertEqual(len(m["payloads"]), 1, "AppImages absent: only Windows")
        p = m["payloads"][0]
        self.assertEqual((p["platform"], p["arch"]), ("windows", "x86_64"))
        self.assertTrue(p["full"]["url"].startswith(bum.DOWNLOAD_BASE + "v26.10.3/"))
        self.assertEqual([f["path"] for f in p["files"]], ["AetherSDR.exe", "Qt6Core.dll"])
        (patch,) = p["patches"]
        self.assertEqual(patch["from"], "26.10.2")
        self.assertEqual([o["path"] for o in patch["ops"]], ["AetherSDR.exe"])
        bundle = out / patch["bundle"]["asset"]
        self.assertEqual(bum.sha256_file(bundle), patch["bundle"]["sha256"])
        self.assertTrue(bundle.name.endswith(".update.tar"))
        apply_patch_set(old, bundle, patch, p["files"], self.tmp / "applied")
        self.assertTrue((dirs["v26.10.2"] / "AetherSDR-v26.10.2-Windows-x64-portable.zip").exists(),
                        "local assets are never deleted")


if __name__ == "__main__":
    unittest.main(verbosity=1)
