#!/usr/bin/env python3
"""Tests for check_release_assets.py's asset checks: the 14-file page and the
files Release Offload moves to updates.aethersdr.com (RFC #6964)."""

from __future__ import annotations

import argparse
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_release_assets as c  # noqa: E402

NEW, OLD = "v26.11.4", "v26.11.3"
BUILT, SIGNED = "2026-11-22T10:20:00Z", "2026-11-22T10:40:00Z"


def release(v: str, names: list[str]) -> dict:
    def asset(n: str) -> dict:
        at = SIGNED if n.endswith(".asc") or n.startswith("SHA256SUMS") or n.endswith("source.tar.gz") else BUILT
        return {"name": n, "size": 100, "created_at": at, "uploader": {"login": "github-actions[bot]"}}
    return {"assets": [asset(n) for n in names]}


def full(v: str) -> list[str]:
    return c.expected_assets(v, hotfix=False) + c.expected_symbol_assets(v)


class Checked(unittest.TestCase):
    def setUp(self):
        c.results.clear()
        self.on_r2: set[str] = set()
        self._r2 = c.r2_size
        c.r2_size = lambda v, n: 100 if n in self.on_r2 else None
        self._print = print
        c.print = lambda *a, **k: None

    def tearDown(self):
        c.r2_size = self._r2
        del c.print

    def check(self, v: str, names: list[str], offloaded: bool):
        return c.check_assets(release(v, names), v, False, True, True, True, offloaded)

    def statuses(self) -> dict[str, str]:
        return {name: status for status, name, _ in c.results}

    def status_of(self, prefix: str) -> str:
        found = [s for n, s in self.statuses().items() if n.startswith(prefix)]
        self.assertEqual(len(found), 1, (prefix, self.statuses()))
        return found[0]


class Offloaded(Checked):
    def moved(self) -> list[str]:
        return c.moved_off_page(NEW, full(NEW))

    def test_the_page_is_fourteen_files(self):
        page = [n for n in full(NEW) if n not in self.moved()]
        self.assertEqual(len(page), 14)

    def test_after_the_move_page_and_r2_pass(self):
        self.on_r2 = set(self.moved())
        page = [n for n in full(NEW) if n not in self.on_r2]
        _, on_r2 = self.check(NEW, page, True)
        self.assertEqual(self.status_of("the 14-asset page"), "PASS")
        self.assertEqual(self.status_of("the 15 files Release Offload moves"), "PASS")
        self.assertEqual(self.status_of(".msixupload version"), "PASS")
        self.assertEqual(sorted(on_r2), sorted(self.moved()))
        self.assertNotIn("FAIL", self.statuses().values())
        self.assertNotIn("WARN", self.statuses().values())

    def test_before_the_move_it_warns_and_does_not_fail(self):
        self.check(NEW, full(NEW), True)
        self.assertEqual(self.status_of("Release Offload has moved"), "WARN")
        self.assertEqual(self.status_of("the 14-asset page"), "PASS")
        self.assertNotIn("FAIL", self.statuses().values())

    def test_a_signature_on_neither_fails(self):
        lost = f"AetherSDR-{NEW}-x86_64.AppImage.asc"
        self.on_r2 = set(self.moved()) - {lost}
        page = [n for n in full(NEW) if n not in self.moved()]
        self.check(NEW, page, True)
        self.assertEqual(self.status_of("the 10 signatures and packages"), "FAIL")

    def test_a_symbol_archive_on_neither_only_warns(self):
        lost = f"AetherSDR-{NEW}-Windows-x64-symbols.tar.xz"
        self.on_r2 = set(self.moved()) - {lost}
        page = [n for n in full(NEW) if n not in self.moved()]
        self.check(NEW, page, True)
        self.assertEqual(self.status_of("the 5 debug-symbol archives"), "WARN")
        self.assertNotIn("FAIL", self.statuses().values())

    def test_a_copy_of_another_size_on_r2_is_not_the_move(self):
        self.on_r2 = set(self.moved())
        c.r2_size = lambda v, n: 99 if n in self.on_r2 else None
        _, on_r2 = self.check(NEW, full(NEW), True)
        self.assertEqual(self.status_of("Release Offload has moved"), "WARN")
        self.assertEqual(on_r2, [])

    def test_patch_bundles_on_the_page_warn(self):
        self.on_r2 = set(self.moved())
        page = [n for n in full(NEW) if n not in self.on_r2] + [f"AetherSDR-{NEW}-x86_64-from-v26.11.3.update.tar"]
        self.check(NEW, page, True)
        self.assertEqual(self.status_of("no patch bundles on the page"), "WARN")


class OffloadedEdges(Checked):
    def moved(self, v=NEW, hotfix=False):
        return c.moved_off_page(v, c.expected_assets(v, hotfix) + c.expected_symbol_assets(v))

    def page(self, v=NEW, hotfix=False):
        return [n for n in c.expected_assets(v, hotfix) if n not in self.moved(v, hotfix)]

    def test_a_binary_attached_after_the_signing_fails(self):
        self.on_r2 = set(self.moved())
        rel = release(NEW, self.page())
        for a in rel["assets"]:
            if a["name"].endswith("Windows-x64-setup.exe"):
                a["created_at"] = "2026-11-22T11:00:00Z"
        c.check_assets(rel, NEW, False, True, True, True, True)
        self.assertEqual(self.status_of("every signed binary predates SHA256SUMS.txt"), "FAIL")

    def test_signed_binaries_older_than_the_sums_pass(self):
        self.on_r2 = set(self.moved())
        self.check(NEW, self.page(), True)
        self.assertEqual(self.status_of("every signed binary predates SHA256SUMS.txt"), "PASS")

    def test_an_empty_copy_on_r2_is_not_there(self):
        self.on_r2 = set(self.moved())
        c.r2_size = lambda v, n: 0 if n.endswith("msixupload") else (100 if n in self.on_r2 else None)
        self.check(NEW, self.page(), True)
        self.assertEqual(self.status_of("the 10 signatures and packages"), "FAIL")

    def test_an_unreachable_r2_warns_and_does_not_call_files_lost(self):
        c.r2_size = lambda v, n: c.UNKNOWN
        self.check(NEW, self.page(), True)
        self.assertEqual(self.status_of("updates.aethersdr.com answered"), "WARN")
        self.assertNotIn("FAIL", self.statuses().values())

    def test_a_hotfix_msixupload_moved_to_r2_fails(self):
        v = "v26.11.4.1"
        self.on_r2 = set(self.moved(v, True)) | {"AetherSDR-26.11.4.1-Windows-x64.msixupload"}
        c.check_assets(release(v, self.page(v, True)), v, True, True, True, True, True)
        self.assertEqual(self.status_of(".msixupload absent for a hotfix"), "FAIL")

    def test_a_hotfix_without_one_passes(self):
        v = "v26.11.4.1"
        self.on_r2 = set(self.moved(v, True))
        c.check_assets(release(v, self.page(v, True)), v, True, True, True, True, True)
        self.assertNotIn("FAIL", self.statuses().values())
        self.assertEqual(self.status_of("the 14 files Release Offload moves"), "PASS")

    def test_a_four_part_release_tag_finds_its_msixupload(self):
        v = "v26.11.4.0"
        self.on_r2 = set(self.moved(v))
        self.check(v, self.page(v), True)
        self.assertEqual(self.status_of(".msixupload version"), "PASS")


class BeforeTheOffload(Checked):
    def test_the_bridge_and_older_keep_the_full_set(self):
        self.assertFalse(c.release_offloaded(OLD))
        self.assertTrue(c.release_offloaded(NEW))
        self.check(OLD, full(OLD) + [f"AetherSDR-{OLD}-x86_64-from-v26.11.2.update.tar"], False)
        self.assertEqual(self.status_of("the 24-asset set"), "PASS")
        self.assertEqual(self.status_of("patch bundles on the page"), "INFO")
        self.assertNotIn("WARN", self.statuses().values())


class Downloads(unittest.TestCase):
    def test_signatures_on_r2_are_fetched_from_there(self):
        c.results.clear()
        fetched: list[str] = []
        moved = c.moved_off_page(NEW, full(NEW))
        page = {n: {} for n in full(NEW) if n not in moved}

        def fake_run(argv, check=True):
            ddir = Path(argv[argv.index("--dir") + 1])
            for i, a in enumerate(argv):
                if a == "--pattern":
                    (ddir / argv[i + 1]).write_bytes(b"x")
            return subprocess.CompletedProcess(argv, 0, "", "")

        def fake_fetch(v, name, dest):
            fetched.append(name)
            dest.write_bytes(b"x")
            return True
        orig = c.run, c.r2_fetch, c.shutil.which
        c.run, c.r2_fetch = fake_run, fake_fetch
        c.shutil.which = lambda n: None if n == "gpg" else "/usr/bin/" + n
        c.print = lambda *a, **k: None
        try:
            with tempfile.TemporaryDirectory() as d:
                args = argparse.Namespace(skip_download=False, download_dir=d, all=False, key=None)
                c.check_downloads("aethersdr/AetherSDR", NEW, page, args, True, True, True, moved)
        finally:
            c.run, c.r2_fetch, c.shutil.which = orig
            del c.print
        self.assertIn(f"AetherSDR-{NEW}-x86_64.AppImage.asc", fetched)
        self.assertIn(f"AetherSDR-{NEW}-source.tar.gz.asc", fetched)
        self.assertNotIn("SHA256SUMS.txt.asc", fetched)


if __name__ == "__main__":
    unittest.main()
