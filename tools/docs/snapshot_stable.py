#!/usr/bin/env python3
"""Replace the Stable docs with a snapshot of the current docs.

Usage:
    python3 tools/docs/snapshot_stable.py 26.10.2    # or v26.10.2

The docs site has two versions (docs/user/docusaurus.config.js):

  Stable       docs/user/versioned_docs/version-stable/, served at /, the
               default. It describes the latest release.
  Next (main)  docs/user/docs/, served at /next/. It tracks main.

There is only ever one snapshot. At release prep (/release-prep) this script
makes the current docs the new Stable:

  * versioned_docs/version-stable/ becomes a copy of docs/user/docs/;
  * versioned_sidebars/version-stable-sidebars.json becomes the evaluated
    sidebars.js (sidebar-extra.json included);
  * versions.json is ["stable"];
  * stable-version.json's label becomes the version, which the version menu
    shows;
  * each translation's i18n/<locale>/docusaurus-plugin-content-docs/current/
    is copied to version-stable/ beside it. Re-run
    `npm run write-translations -- --locale <locale>` afterwards for the
    sidebar and version labels.

This is what `docusaurus docs:version` does, minus its refusal to overwrite an
existing version, and it needs no npm install: only node, to read
sidebars.js. Run `npm run build` in docs/user afterwards.

Stdlib only; Python 3.9+.
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SITE = REPO / "docs" / "user"
DOCS = SITE / "docs"
VERSION = "stable"
VERSIONED_DOCS = SITE / "versioned_docs" / f"version-{VERSION}"
VERSIONED_SIDEBARS = SITE / "versioned_sidebars" / f"version-{VERSION}-sidebars.json"
VERSIONS_JSON = SITE / "versions.json"
LABEL_JSON = SITE / "stable-version.json"
I18N = SITE / "i18n"


def fail(msg: str) -> None:
    print(f"error: {msg}", file=sys.stderr)
    sys.exit(1)


def read_sidebars(node: str) -> dict:
    script = (
        "import(process.argv[1]).then(m => "
        "process.stdout.write(JSON.stringify(m.default)))"
        ".catch(e => { console.error(e); process.exit(1); });"
    )
    url = (SITE / "sidebars.js").resolve().as_uri()
    try:
        out = subprocess.run([node, "-e", script, url], check=True,
                             capture_output=True, text=True)
    except FileNotFoundError:
        fail(f"node not found ({node}); it is needed to read sidebars.js")
    except subprocess.CalledProcessError as e:
        fail(f"could not read sidebars.js:\n{e.stderr}")
    return json.loads(out.stdout)


def replace_tree(src: Path, dest: Path) -> None:
    """Copy src over dest, swapping only once the copy is complete."""
    tmp = dest.with_name(dest.name + ".tmp")
    if tmp.exists():
        shutil.rmtree(tmp)
    shutil.copytree(src, tmp)
    if dest.exists():
        shutil.rmtree(dest)
    tmp.rename(dest)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("version", help="the release the snapshot describes, e.g. 26.10.2")
    ap.add_argument("--node", default="node", help="node executable (default: node)")
    args = ap.parse_args()

    label = args.version.lstrip("v")
    if not re.fullmatch(r"\d{2}\.\d{1,2}\.\d+(\.\d+)?", label):
        fail(f"{args.version} is not a CalVer version (YY.M.patch[.hotfix])")
    if not DOCS.is_dir():
        fail(f"{DOCS} does not exist")

    sidebars = read_sidebars(args.node)

    replace_tree(DOCS, VERSIONED_DOCS)
    VERSIONED_SIDEBARS.parent.mkdir(parents=True, exist_ok=True)
    VERSIONED_SIDEBARS.write_text(json.dumps(sidebars, indent=2, ensure_ascii=False) + "\n",
                                  encoding="utf-8")
    VERSIONS_JSON.write_text(json.dumps([VERSION]) + "\n", encoding="utf-8")
    LABEL_JSON.write_text(json.dumps({"label": label}, indent=2) + "\n", encoding="utf-8")

    pages = sum(1 for _ in VERSIONED_DOCS.rglob("*.md"))
    print(f"stable snapshot ({label}): {pages} pages -> {VERSIONED_DOCS.relative_to(REPO)}")
    print(f"wrote {VERSIONED_SIDEBARS.relative_to(REPO)}, "
          f"{VERSIONS_JSON.relative_to(REPO)}, {LABEL_JSON.relative_to(REPO)}")

    if I18N.is_dir():
        for current in sorted(I18N.glob("*/docusaurus-plugin-content-docs/current")):
            replace_tree(current, current.with_name(f"version-{VERSION}"))
            locale = current.parts[-3]
            print(f"i18n {locale}: copied current/ to version-{VERSION}/; run "
                  f"`npm run write-translations -- --locale {locale}` for the labels")
    return 0


if __name__ == "__main__":
    sys.exit(main())
