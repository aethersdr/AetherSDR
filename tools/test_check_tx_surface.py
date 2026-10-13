#!/usr/bin/env python3
"""
Self-test for tools/check_tx_surface.py.

Each case builds a throwaway git repo carrying the real .github/tx-surface.txt,
commits a base, applies one change on top, and runs the checker against it.
The gate is only worth having if it fails on a keying change without an ack,
passes with one, and stays quiet on a comment-only or unrelated change.

Usage:
    python tools/test_check_tx_surface.py
"""

from __future__ import annotations

import contextlib
import io
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_tx_surface  # noqa: E402

SURFACE_TEXT = (check_tx_surface.REPO / check_tx_surface.SURFACE).read_text()
PR = 4242
ACK = f"{check_tx_surface.ACK_DIR}/{PR}.md"

BASE_FILES = {
    "src/models/RadioModel.cpp": "void f() {\n    int x = 0;\n}\n",
    "src/models/TxController.cpp": "// controller\n",
    "src/gui/Label.cpp": "void g() {}\n",
}

# (name, files to write after the base commit, expected exit code, substring
# the output must contain)
CASES = [
    ("unrelated change passes",
     {"src/gui/Label.cpp": "void g() { int y = 1; }\n"}, 0, "not touched"),
    ("keying token without ack fails",
     {"src/models/RadioModel.cpp": "void f() {\n    sendCmd(\"xmit 1\");\n}\n"}, 1, ACK),
    ("removed keying token without ack fails",
     None, 1, "- line uses"),
    ("keying token with ack passes",
     {"src/models/RadioModel.cpp": "void f() {\n    m_backend->setKeying(true);\n}\n",
      ACK: "Keys MOX through setKeying; verified on the demo radio.\n"}, 0, "Acknowledged"),
    ("empty ack fails",
     {"src/models/RadioModel.cpp": "void f() {\n    m_backend->setKeying(true);\n}\n",
      ACK: "\n"}, 1, "is empty"),
    ("comment-only mention passes",
     {"src/models/RadioModel.cpp": "void f() {\n    // was sendCmd(\"xmit 1\")\n    int x = 0;\n}\n"},
     0, "not touched"),
    ("path entry without ack fails",
     {"src/models/TxController.cpp": "// controller\nint grants = 0;\n"}, 1, "file is on the TX surface"),
    ("comment-only edit in a path-listed file passes",
     {"src/models/TxController.cpp": "// controller, reworded\n"}, 0, "not touched"),
    ("preprocessor line is code, not a comment",
     {"src/models/RadioModel.cpp": "#define KEY \"xmit 1\"\nvoid f() {\n    int x = 0;\n}\n"},
     1, ACK),
    ("code after a closed block comment is code",
     {"src/models/RadioModel.cpp": "void f() {\n    /* key */ sendCmd(\"xmit 1\");\n}\n"}, 1, ACK),
    ("UI string naming the env var passes",
     {"src/gui/Label.cpp": "void g() { tip(\"set AETHER_AUTOMATION_ALLOW_TX=1\"); }\n"},
     0, "not touched"),
    ("token outside src/ passes",
     {"docs/notes.md": "setKeying is the entry point\n"}, 0, "not touched"),
]


def git(repo: Path, *args: str) -> None:
    subprocess.run(["git", "-C", str(repo), *args], check=True, capture_output=True)


def write(repo: Path, files: dict[str, str]) -> None:
    for rel, text in files.items():
        p = repo / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text)


def run_case(name: str, change: dict[str, str] | None, want: int, needle: str) -> bool:
    with tempfile.TemporaryDirectory() as tmp:
        repo = Path(tmp)
        git(repo, "init", "-q", "-b", "main")
        git(repo, "config", "user.email", "t@example.invalid")
        git(repo, "config", "user.name", "t")
        git(repo, "config", "commit.gpgsign", "false")
        base_files = dict(BASE_FILES)
        if change is None:
            # A removal: the base already keys, the change takes it out.
            base_files["src/models/RadioModel.cpp"] = "void f() {\n    sendCmd(\"xmit 0\");\n}\n"
            change = {"src/models/RadioModel.cpp": BASE_FILES["src/models/RadioModel.cpp"]}
        write(repo, {check_tx_surface.SURFACE: SURFACE_TEXT, **base_files})
        git(repo, "add", "-A")
        git(repo, "commit", "-q", "-m", "base")
        write(repo, change)
        git(repo, "add", "-A")
        git(repo, "commit", "-q", "-m", "change")

        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            got = check_tx_surface.main(["--repo", str(repo), "--base", "HEAD^1", "--pr", str(PR)])
        text = out.getvalue()
        ok = got == want and needle in text
        print(f"{'PASS' if ok else 'FAIL'}: {name}")
        if not ok:
            print(f"  exit {got}, wanted {want}; output must contain {needle!r}:")
            print("  " + text.replace("\n", "\n  "))
        return ok


def main() -> int:
    check_tx_surface.load_surface(SURFACE_TEXT)  # the real list must parse
    results = [run_case(*c) for c in CASES]
    print(f"\n{sum(results)}/{len(results)} cases passed")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
