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


class ReleaseTarball(Base):
    """The Linux release tarball, read back by the contract's own reader."""

    def tree(self) -> Path:
        root = self.tmp / "tree"
        write(root / "usr/bin/AetherSDR", b"elf" * 3000, executable=True)
        write(root / "usr/lib/libQt6Core.so.6.12.0", b"qt" * 5000)
        os.symlink("libQt6Core.so.6.12.0", root / "usr/lib/libQt6Core.so.6")
        os.symlink("usr/bin/AetherSDR", root / "AppRun")
        deep = "usr/share/" + "/".join(["d" * 40] * 3) + "/icon.png"   # needs ustar's prefix field
        write(root / deep, b"png")
        (root / "usr/share/empty").mkdir(parents=True)
        return root

    def raw_tar(self, out: Path, members: list[tarfile.TarInfo | tuple[tarfile.TarInfo, bytes]],
                fmt: int = tarfile.USTAR_FORMAT) -> Path:
        tar_path = out.with_suffix(".tar")
        with tarfile.open(tar_path, "w", format=fmt) as tar:
            for m in members:
                if isinstance(m, tuple):
                    import io
                    tar.addfile(m[0], io.BytesIO(m[1]))
                else:
                    tar.addfile(m)
        subprocess.run(["zstd", "-q", "-f", str(tar_path), "-o", str(out)], check=True)
        return out

    def test_round_trip_and_determinism(self) -> None:
        root = self.tree()
        tree = bum.scan_tree(root)
        a, b = self.tmp / "a.tar.zst", self.tmp / "b.tar.zst"
        bum.write_release_tarball(root, tree, a)
        bum.write_release_tarball(root, tree, b)
        self.assertEqual(a.read_bytes(), b.read_bytes(), "bytes depend only on the tree")
        files = bum.files_list(tree)
        bum.check_release_tarball(a, files)
        # What a user's tar sees: the same tree, exec bit and links included,
        # and no empty folder.
        out = self.tmp / "x"
        out.mkdir()
        subprocess.run(["tar", "--zstd", "-xf", str(a), "-C", str(out)], check=True)
        self.assertEqual(bum.scan_tree(out), tree)
        self.assertTrue(os.access(out / "usr/bin/AetherSDR", os.X_OK))
        self.assertFalse((out / "usr/share/empty").exists())

    def test_every_difference_from_the_manifest_is_refused(self) -> None:
        root = self.tree()
        tree = bum.scan_tree(root)
        files = bum.files_list(tree)
        good = self.tmp / "good.tar.zst"
        bum.write_release_tarball(root, tree, good)

        def entry(name: str, kind: bytes = tarfile.REGTYPE, size: int = 0) -> tarfile.TarInfo:
            i = tarfile.TarInfo(name)
            i.type, i.size = kind, size
            return i

        def refused(path: Path, why: str) -> None:
            with self.assertRaises(ValueError, msg=why):
                bum.check_release_tarball(path, files)

        # One file missing from the archive, and one extra.
        refused(self.raw_tar(self.tmp / "one.tar.zst", [(entry("AppRun.txt", size=1), b"x")]), "missing entries")
        fewer = [f for f in files if f["path"] != "AppRun"]
        with self.assertRaises(ValueError, msg="extra entry"):
            bum.check_release_tarball(good, fewer)
        # A file whose bytes differ from its signed hash.
        stale = [dict(f, sha256="0" * 64) if f["path"] == "usr/bin/AetherSDR" else f for f in files]
        with self.assertRaises(ValueError, msg="stale content"):
            bum.check_release_tarball(good, stale)
        # A link to somewhere else, a hard link, a pax header.
        moved = [dict(f, target="elsewhere") if f["path"] == "AppRun" else f for f in files]
        with self.assertRaises(ValueError, msg="link target"):
            bum.check_release_tarball(good, moved)
        hard = entry("AppRun", tarfile.LNKTYPE)
        hard.linkname = "usr/bin/AetherSDR"
        refused(self.raw_tar(self.tmp / "hard.tar.zst", [hard]), "hard link")
        refused(self.raw_tar(self.tmp / "pax.tar.zst", [(entry("AppRun.txt", size=1), b"x")],
                             tarfile.PAX_FORMAT), "pax")
        # Data after the end-of-archive blocks.
        tar_path = self.tmp / "tail.tar"
        subprocess.run(["zstd", "-q", "-d", "-f", str(good), "-o", str(tar_path)], check=True)
        with tar_path.open("ab") as f:
            f.write(b"\0" * 1024 + b"junk" + b"\0" * 508)
        tail = self.tmp / "tail.tar.zst"
        subprocess.run(["zstd", "-q", "-f", str(tar_path), "-o", str(tail)], check=True)
        refused(tail, "data after the end")
        # Cut short.
        cut = self.tmp / "cut.tar"
        cut.write_bytes(tar_path.read_bytes()[:4096])
        subprocess.run(["zstd", "-q", "-f", str(cut), "-o", str(self.tmp / "cut.tar.zst")], check=True)
        refused(self.tmp / "cut.tar.zst", "cut short")
        # A GNU header where ustar is required: the first header's magic
        # rewritten, its checksum made good again.
        raw = bytearray(tar_path.read_bytes()[:-1536])
        raw[257:265] = b"ustar  \0"
        raw[148:156] = b" " * 8
        raw[148:156] = b"%06o\0 " % sum(raw[:512])
        gnu = self.tmp / "gnu.tar"
        gnu.write_bytes(bytes(raw) + b"\0" * 1024)
        subprocess.run(["zstd", "-q", "-f", str(gnu), "-o", str(self.tmp / "gnu.tar.zst")], check=True)
        refused(self.tmp / "gnu.tar.zst", "GNU magic")
        # A header whose checksum does not add up.
        raw = bytearray(tar_path.read_bytes()[:-1536])
        raw[148:156] = b"%06o\0 " % (sum(raw[:148]) + 8 * 32 + sum(raw[156:512]) + 1)
        sums = self.tmp / "sum.tar"
        sums.write_bytes(bytes(raw) + b"\0" * 1024)
        subprocess.run(["zstd", "-q", "-f", str(sums), "-o", str(self.tmp / "sum.tar.zst")], check=True)
        refused(self.tmp / "sum.tar.zst", "checksum")
        # Everything but one entry.
        part = {k: e for k, e in tree.items() if k != "usr/lib/libQt6Core.so.6"}
        bum.write_release_tarball(root, part, self.tmp / "part.tar.zst")
        refused(self.tmp / "part.tar.zst", "an entry missing")
        # Another compressor's stream named .tar.zst, and zstd followed by
        # gzip: zstd -d decodes both, the app neither.
        import gzip
        disguised = self.tmp / "gz.tar.zst"
        disguised.write_bytes(gzip.compress(tar_path.read_bytes(), mtime=0))
        refused(disguised, "gzip")
        mixed = self.tmp / "mixed.tar.zst"
        mixed.write_bytes(good.read_bytes() + gzip.compress(b"\0" * 1024, mtime=0))
        refused(mixed, "zstd then gzip")
        # A window larger than the app's 2^27 (streamed, so zstd keeps it).
        wide = self.tmp / "wide.tar.zst"
        with tar_path.open("rb") as src, wide.open("wb") as dst:
            subprocess.run(["zstd", "-q", "--long=28", "-c"], stdin=src, stdout=dst, check=True)
        refused(wide, "window")

    def test_frames_the_app_also_decodes_are_accepted(self) -> None:
        root = self.tree()
        tree = bum.scan_tree(root)
        good = self.tmp / "good.tar.zst"
        bum.write_release_tarball(root, tree, good)
        # A skippable frame first, and the archive split over two frames.
        skip = self.tmp / "skip.tar.zst"
        skip.write_bytes(b"\x50\x2a\x4d\x18\x04\x00\x00\x00abcd" + good.read_bytes())
        bum.check_release_tarball(skip, bum.files_list(tree))
        tar_path = self.tmp / "t.tar"
        subprocess.run(["zstd", "-q", "-d", "-f", str(good), "-o", str(tar_path)], check=True)
        raw = tar_path.read_bytes()
        halves = [self.tmp / "h1", self.tmp / "h2"]
        halves[0].write_bytes(raw[:len(raw) // 2])
        halves[1].write_bytes(raw[len(raw) // 2:])
        two = self.tmp / "two.tar.zst"
        with two.open("wb") as out:
            for h in halves:
                out.write(subprocess.run(["zstd", "-q", "-c", str(h)], check=True,
                                         stdout=subprocess.PIPE).stdout)
        bum.check_release_tarball(two, bum.files_list(tree))

    def test_a_name_ustar_cannot_hold_fails_the_build(self) -> None:
        root = self.tmp / "long"
        write(root / ("x" * 120), b"a")
        with self.assertRaises(ValueError):
            bum.write_release_tarball(root, bum.scan_tree(root), self.tmp / "long.tar.zst")


class LinuxFull(Base):
    """A Linux payload's `full` is its release tarball, once it matches."""

    def setUp(self) -> None:
        super().setUp()
        self.root = ReleaseTarball.tree(self)
        self.real_extract = bum.extract_payload
        # The AppImage's payload, without unsquashfs: its tree is this folder.
        bum.extract_payload = lambda asset, container, dest: (
            self.root if container == "appimage" else self.real_extract(asset, container, dest))
        self.dl = self.tmp / "dl"
        write(self.dl / "AetherSDR-v26.10.4-x86_64.AppImage", b"\x7fELF image")

    def tearDown(self) -> None:
        bum.extract_payload = self.real_extract
        super().tearDown()

    def run_build(self, require_all: bool = False) -> dict:
        get = lambda t, n: (self.dl / n) if (self.dl / n).exists() else None  # noqa: E731
        return bum.build("v26.10.4", self.tmp / "out", 0, get, [], None, self.tmp / "w",
                         require_all=require_all)

    def test_the_tarball_is_full(self) -> None:
        bum.write_release_tarball(self.root, bum.scan_tree(self.root), self.dl / "AetherSDR-v26.10.4-x86_64.tar.zst")
        (p,) = self.run_build()["payloads"]
        self.assertEqual(p["full"]["asset"], "AetherSDR-v26.10.4-x86_64.tar.zst")
        self.assertEqual(p["full"]["url"], bum.DOWNLOAD_BASE + "v26.10.4/AetherSDR-v26.10.4-x86_64.tar.zst")
        self.assertEqual(p["full"]["sha256"], bum.sha256_file(self.dl / "AetherSDR-v26.10.4-x86_64.tar.zst"))

    def test_a_tarball_of_another_tree_fails_the_build(self) -> None:
        other = self.tmp / "other"
        write(other / "AppRun", b"not the release")
        bum.write_release_tarball(other, bum.scan_tree(other), self.dl / "AetherSDR-v26.10.4-x86_64.tar.zst")
        with self.assertRaises(ValueError):
            self.run_build()

    def test_no_tarball_fails_a_release_build(self) -> None:
        with self.assertRaises(bum.MissingPayload):
            self.run_build(require_all=True)
        # A local partial build keeps the AppImage as full.
        (p,) = self.run_build()["payloads"]
        self.assertEqual(p["full"]["asset"], "AetherSDR-v26.10.4-x86_64.AppImage")

    def test_the_cli_makes_and_checks_a_tarball(self) -> None:
        out = self.tmp / "AetherSDR-v26.10.4-x86_64.tar.zst"
        self.assertEqual(bum.main(["--tarball", str(self.dl / "AetherSDR-v26.10.4-x86_64.AppImage"), str(out),
                                   "--work", str(self.tmp / "tw")]), 0)
        bum.check_release_tarball(out, bum.files_list(bum.scan_tree(self.root)))


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
