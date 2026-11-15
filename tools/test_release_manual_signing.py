#!/usr/bin/env python3
"""Tests for release_manual_signing.py: the manual's signature shape, and the
re-sign decision after Docs attaches or replaces the manual (RFC #6964)."""

from __future__ import annotations

import hashlib
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import release_manual_signing as m  # noqa: E402

NAME = "AetherSDR-Manual-v26.11.3.pdf"


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sums(*entries: tuple[str, str]) -> str:
    return "".join(f"{h}  {n}\n" for h, n in entries)


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
        old = "run: gpg --batch --detach-sign --armor \"AetherSDR-Manual-${TAG}.pdf\"\n"
        self.assertEqual(m.shape(self.tree(old)), "apart")

    def test_a_tag_whose_docs_workflow_only_attaches_it_lists_it_in_the_sums(self):
        self.assertEqual(m.shape(self.tree("run: gh release upload \"$TAG\" pdf --clobber\n")), "sums")

    def test_a_tag_without_the_docs_site_has_no_manual(self):
        self.assertEqual(m.shape(self.tree("--detach-sign", site=False)), "none")
        self.assertEqual(m.shape(self.tree(None, site=False)), "none")


class Resign(unittest.TestCase):
    pdf = b"%PDF-1.7 manual of the 16th"
    other = b"%PDF-1.7 manual of the 17th"
    exe = sha(b"setup")

    def test_docs_before_any_signing_waits_for_the_build_triggered_runs(self):
        self.assertEqual(m.resign(NAME, sha(self.pdf), None), "skip: nothing is signed yet")

    def test_failed_docs_then_signing_without_the_manual_then_a_successful_retry(self):
        signed_without = sums((self.exe, "AetherSDR-v26.11.3-Windows-x64-setup.exe"))
        self.assertEqual(m.resign(NAME, sha(self.pdf), signed_without), "resign")

    def test_a_docs_rerun_after_signing_that_replaces_the_manual(self):
        signed = sums((self.exe, "AetherSDR-v26.11.3-Windows-x64-setup.exe"), (sha(self.pdf), NAME))
        self.assertEqual(m.resign(NAME, sha(self.other), signed), "resign")

    def test_a_docs_rerun_that_reattaches_the_same_bytes(self):
        signed = sums((sha(self.pdf), NAME))
        self.assertEqual(m.resign(NAME, sha(self.pdf), signed), "skip: SHA256SUMS.txt already covers this manual")
        self.assertEqual(m.resign(NAME, sha(self.pdf).upper(), signed),
                         "skip: SHA256SUMS.txt already covers this manual")

    def test_binary_mode_lines_and_other_manuals(self):
        self.assertEqual(m.sums_entry(f"{sha(self.pdf)} *{NAME}\n", NAME), sha(self.pdf))
        other_manual = sums((sha(self.pdf), "AetherSDR-Manual-v26.11.2.pdf"))
        self.assertEqual(m.resign(NAME, sha(self.pdf), other_manual), "resign")

    def test_no_manual_attached(self):
        self.assertEqual(m.resign(NAME, None, sums((sha(self.pdf), NAME))), "skip: the manual is not attached")

    def test_the_command_line(self):
        d = Path(self.enterContext(tempfile.TemporaryDirectory()))
        (d / NAME).write_bytes(self.other)
        (d / "SHA256SUMS.txt").write_text(sums((sha(self.pdf), NAME)))
        import contextlib
        import io
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            m.main(["resign", "--name", NAME, "--pdf", str(d / NAME), "--sums", str(d / "SHA256SUMS.txt")])
        self.assertEqual(out.getvalue().strip(), "resign")
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            m.main(["resign", "--name", NAME, "--pdf", str(d / NAME), "--sums", str(d / "absent.txt")])
        self.assertEqual(out.getvalue().strip(), "skip: nothing is signed yet")


if __name__ == "__main__":
    unittest.main()
