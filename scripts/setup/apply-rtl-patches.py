#!/usr/bin/env python3
"""Apply the hash-pinned RTL lifetime patch, including from shipped sources."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys


def apply(source):
    recipe = Path(__file__).resolve().parent
    pin = json.loads((recipe / "rtl-dependencies.json").read_text())["rtl"]
    patch = recipe / pin["patch"]
    if hashlib.sha256(patch.read_bytes()).hexdigest() != pin["patchSha256"]:
        raise RuntimeError("RTL patch SHA-256 does not match rtl-dependencies.json")
    source = Path(source).resolve()
    # Extracted sources can live beneath an AetherSDR worktree. Do not let git
    # interpret their paths relative to the enclosing repository.
    env = dict(os.environ, GIT_CEILING_DIRECTORIES=str(source.parent))
    command = ["git", "-C", str(source), "apply", "--whitespace=error", str(patch)]
    checked = subprocess.run(command[:-1] + ["--check", command[-1]], env=env,
                             capture_output=True, text=True)
    if checked.returncode:
        # The vendor's original lines include trailing whitespace, so a
        # reverse check must not reject those lines as newly added whitespace.
        reverse = subprocess.run(["git", "-C", str(source), "apply", "--reverse",
                                  "--check", "--whitespace=nowarn", str(patch)],
                                 env=env, capture_output=True, text=True)
        if reverse.returncode == 0:
            return
        raise RuntimeError("Pinned RTL patch does not apply: " + checked.stderr)
    subprocess.run(command, env=env, check=True)


if __name__ == "__main__":
    apply(sys.argv[1])
