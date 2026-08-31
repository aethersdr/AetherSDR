#!/usr/bin/env python3
"""Regression guard for gen_bridge_docs.py's generalised sub-action check.

The verb-table drift check has its own CI test (`bridge_docs_check`). This one
pins the part that used to be `slice`-only: `pan` and `audioCapture` sub-actions
are now audited against `panActionList()` / `audioCaptureActionList()` too, and
a doc that stops documenting one of them must fail `--check`.

Pure Python, no app/Qt. Mutates docs/automation-bridge.md in place and restores
it under try/finally.
"""

import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
DOCS = os.path.join(REPO, "docs", "automation-bridge.md")
SCRIPT = os.path.join(HERE, "gen_bridge_docs.py")


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def run_check():
    return subprocess.run(
        [sys.executable, SCRIPT, "--check"],
        cwd=REPO, capture_output=True, text=True)


def test_clean_tree_passes():
    r = run_check()
    check(r.returncode == 0, f"--check failed on a clean tree:\n{r.stdout}\n{r.stderr}")
    check("sub-actions across 3 verbs" in r.stdout,
          f"--check no longer reports the multi-verb audit:\n{r.stdout}")


def test_removing_a_pan_action_row_fails_check():
    original = open(DOCS, encoding="utf-8").read()
    # Drop the `rfgain` row from the `### `pan`` action table.
    broken = re.sub(r'^\| `rfgain` \|.*\n', "", original, count=1, flags=re.M)
    check(broken != original, "expected a `| `rfgain` |` row to remove")
    try:
        open(DOCS, "w", encoding="utf-8").write(broken)
        r = run_check()
        check(r.returncode == 1, f"--check should fail with a pan row missing:\n{r.stdout}")
        check("`pan`" in r.stderr and "rfgain" in r.stderr,
              f"--check should name the missing `pan` action:\n{r.stderr}")
    finally:
        open(DOCS, "w", encoding="utf-8").write(original)


def test_removing_an_audiocapture_action_row_fails_check():
    original = open(DOCS, encoding="utf-8").read()
    broken = re.sub(r'^\| `probeNr2Stereo` \|.*\n', "", original, count=1, flags=re.M)
    check(broken != original, "expected a `| `probeNr2Stereo` |` row to remove")
    try:
        open(DOCS, "w", encoding="utf-8").write(broken)
        r = run_check()
        check(r.returncode == 1,
              f"--check should fail with an audioCapture row missing:\n{r.stdout}")
        check("`audioCapture`" in r.stderr and "probeNr2Stereo" in r.stderr,
              f"--check should name the missing `audioCapture` action:\n{r.stderr}")
    finally:
        open(DOCS, "w", encoding="utf-8").write(original)


if __name__ == "__main__":
    failures = 0
    for name, fn in sorted(globals().items()):
        if not name.startswith("test_") or not callable(fn):
            continue
        try:
            fn()
            print(f"ok: {name}")
        except AssertionError as exc:
            failures += 1
            print(f"FAIL: {name}: {exc}", file=sys.stderr)
    print("ALL PASS" if failures == 0 else f"{failures} FAILED")
    sys.exit(1 if failures else 0)
