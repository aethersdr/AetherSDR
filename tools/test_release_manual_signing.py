#!/usr/bin/env python3
"""Tests for release_manual_signing.py: the manual's signature shape, and
whether a run that Docs triggered signs again (RFC #6964)."""

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
import release_manual_signing as m  # noqa: E402

TAG = "v26.11.3"
NAME = f"AetherSDR-Manual-{TAG}.pdf"
OLD_DOCS = 'run: gpg --batch --detach-sign --armor "AetherSDR-Manual-${TAG}.pdf"\n'
NEW_DOCS = 'run: gh release upload "$TAG" "AetherSDR-Manual-${TAG}.pdf" --clobber\n'


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sums(*entries: tuple[str, str]) -> str:
    return "".join(f"{h}  {n}\n" for h, n in entries)


def asset(name: str, at: str) -> dict:
    return {"name": name, "updatedAt": at}


BUILDS = [asset(f"AetherSDR-{TAG}-x86_64.AppImage", "2026-11-20T10:15:00Z"),
          asset(f"AetherSDR-{TAG}-Windows-x64-setup.exe", "2026-11-20T10:30:00Z")]
SIGNED_AT = "2026-11-20T10:40:00Z"


class Shape(unittest.TestCase):
    def tree(self, docs_yml: str | None, site: bool = True) -> Path:
        d = Path(self.enterContext(tempfile.TemporaryDirectory()))
        if site:
            (d / "tools/docs").mkdir(parents=True)
            (d / "tools/docs/build_pdf.py").write_text("")
            (d / "docs/user").mkdir(parents=True)
            (d / "docs/user/sidebars.js").write_text("")
        if docs_yml is not None:
            (d / ".github/workflows").mkdir(parents=True)
            (d / ".github/workflows/docs.yml").write_text(docs_yml)
        return d

    def test_a_tag_whose_docs_workflow_signs_the_manual_keeps_that_shape(self):
        self.assertEqual(m.shape(self.tree(OLD_DOCS)), "apart")

    def test_a_tag_whose_docs_workflow_only_attaches_it_lists_it_in_the_sums(self):
        self.assertEqual(m.shape(self.tree(NEW_DOCS)), "sums")

    def test_a_comment_about_the_old_flow_decides_nothing(self):
        self.assertEqual(m.shape(self.tree("  # it used to run gpg --detach-sign on AetherSDR-Manual\n" + NEW_DOCS)),
                         "sums")
        self.assertEqual(m.shape(self.tree("run: gpg --detach-sign --armor SHA256SUMS.txt\n")), "sums",
                         "a signature of something else is not the manual's")

    def test_a_tag_without_the_docs_site_has_no_manual(self):
        self.assertEqual(m.shape(self.tree(OLD_DOCS, site=False)), "none")
        self.assertEqual(m.shape(self.tree(None, site=False)), "none")


class Plan(unittest.TestCase):
    pdf = b"%PDF-1.7 manual of the 20th"
    other = b"%PDF-1.7 manual of the 21st"

    def signed(self, *extra: dict) -> list[dict]:
        return [*BUILDS, asset(NAME, "2026-11-20T10:02:00Z"), asset("SHA256SUMS.txt", SIGNED_AT), *extra]

    def covering(self, pdf: bytes | None = None) -> str:
        entries = [(sha(b"x"), a["name"]) for a in BUILDS]
        if pdf is not None:
            entries.append((sha(pdf), NAME))
        return sums(*entries)

    def test_docs_before_any_build_waits_for_the_build_triggered_runs(self):
        d = m.plan("sums", [asset(NAME, "2026-11-20T10:02:00Z")], NAME, sha(self.pdf), None)
        self.assertTrue(d.startswith("skip"), d)

    def test_docs_after_builds_with_nothing_signed_signs(self):
        """The Docs run displaced the pending build-triggered run, or every
        earlier run failed: it signs rather than leaving the release unsigned."""
        d = m.plan("sums", [*BUILDS, asset(NAME, "2026-11-20T10:31:00Z")], NAME, sha(self.pdf), None)
        self.assertTrue(d.startswith("resign"), d)

    def test_failed_docs_then_signing_without_the_manual_then_a_successful_retry(self):
        d = m.plan("sums", self.signed(), NAME, sha(self.pdf), self.covering(None))
        self.assertEqual(d, "resign: SHA256SUMS.txt does not list the manual")

    def test_a_docs_rerun_after_signing_that_replaces_the_manual(self):
        d = m.plan("sums", self.signed(), NAME, sha(self.other), self.covering(self.pdf))
        self.assertEqual(d, "resign: the manual was replaced after signing")

    def test_a_docs_rerun_that_reattaches_the_same_bytes(self):
        self.assertTrue(m.plan("sums", self.signed(), NAME, sha(self.pdf), self.covering(self.pdf)).startswith("skip"))
        self.assertTrue(m.plan("sums", self.signed(), NAME, sha(self.pdf).upper(),
                               self.covering(self.pdf)).startswith("skip"))

    def test_a_build_rerun_whose_signing_run_was_displaced(self):
        """A Windows rerun re-attached setup.exe after signing; its pending
        signing run was replaced by this one, so this one must sign."""
        rebuilt = [a if not a["name"].endswith(".exe") else asset(a["name"], "2026-11-20T11:00:00Z") for a in BUILDS]
        assets = [*rebuilt, asset(NAME, "2026-11-20T10:02:00Z"), asset("SHA256SUMS.txt", SIGNED_AT)]
        d = m.plan("sums", assets, NAME, sha(self.pdf), self.covering(self.pdf))
        self.assertEqual(d, f"resign: attached after the last signing: AetherSDR-{TAG}-Windows-x64-setup.exe")

    def test_an_older_release_whose_manual_is_signed_apart_never_resigns(self):
        d = m.plan("apart", self.signed(), NAME, sha(self.pdf), self.covering(None))
        self.assertTrue(d.startswith("skip"), d)

    def test_binary_mode_lines_and_other_manuals(self):
        self.assertEqual(m.sums_entry(f"{sha(self.pdf)} *{NAME}\n", NAME), sha(self.pdf))
        other_manual = sums((sha(self.pdf), "AetherSDR-Manual-v26.11.2.pdf"))
        self.assertEqual(m.plan("sums", self.signed(), NAME, sha(self.pdf), other_manual),
                         "resign: SHA256SUMS.txt does not list the manual")

    def test_signatures_and_the_source_tarball_are_not_build_artifacts(self):
        late = [asset(f"AetherSDR-{TAG}-x86_64.AppImage.asc", "2026-11-20T11:00:00Z"),
                asset(f"AetherSDR-{TAG}-source.tar.gz", "2026-11-20T11:00:00Z")]
        self.assertTrue(m.plan("sums", self.signed(*late), NAME, sha(self.pdf),
                               self.covering(self.pdf)).startswith("skip"))

    def test_the_command_line(self):
        d = Path(self.enterContext(tempfile.TemporaryDirectory()))
        (d / "tree/.github/workflows").mkdir(parents=True)
        (d / "tree/.github/workflows/docs.yml").write_text(NEW_DOCS)
        (d / "tree/tools/docs").mkdir(parents=True)
        (d / "tree/tools/docs/build_pdf.py").write_text("")
        (d / "tree/docs/user").mkdir(parents=True)
        (d / "tree/docs/user/sidebars.js").write_text("")
        (d / NAME).write_bytes(self.other)
        (d / "SHA256SUMS.txt").write_text(self.covering(self.pdf))
        (d / "assets.json").write_text(json.dumps({"assets": self.signed()}))
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            m.main(["plan", "--tree", str(d / "tree"), "--assets", str(d / "assets.json"), "--name", NAME,
                    "--pdf", str(d / NAME), "--sums", str(d / "SHA256SUMS.txt")])
        self.assertEqual(out.getvalue().strip(), "resign: the manual was replaced after signing")


if __name__ == "__main__":
    unittest.main()
