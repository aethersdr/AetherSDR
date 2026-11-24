#!/usr/bin/env python3
"""
Move the release files people don't download from the GitHub release page to
updates.aethersdr.com/releases/<tag>/ (RFC #6964): the per-file .asc
signatures, the debug-symbol archives and the .msixupload. Everything an
operator or a copy of the app downloads stays on the page.

The order is copy, check, then delete. Each file is downloaded from the
release and checked against GitHub's size and SHA-256, then uploaded to R2.
It is then read back from the bucket and checked again, and must be served
at updates.aethersdr.com. Only when every file has passed, and the release
still holds exactly the assets that were copied, are they deleted from the
page. If any copy or check fails, nothing is deleted.

The Release Offload workflow runs this. Its `plan` job has no secrets and a
read-only token, and its `offload` job runs in the update-signing
environment, which holds the bucket's token.

    offload_release_assets.py plan [--tag T] [--dry-run] [--github-output F]
        With no tag, finds the newest release from OFFLOAD_FROM on that still
        has files to move, and says whether it is ready: every build, Docs
        run and signing run has finished, and SHA256SUMS.txt is newer than
        every build artifact it signed. With --tag, plans that tag. With --dry-run, any
        release tag (older ones included) is listed and checked, and nothing
        is written. Writes tag= and ready= to F (ready=false on a dry run).
    offload_release_assets.py offload --tag T --work DIR
        Checks the tag is ready again, then copies, checks and deletes
        (R2_ACCOUNT_ID, R2_ACCESS_KEY_ID, R2_SECRET_ACCESS_KEY, GH_TOKEN,
        GH_REPO).
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from build_update_manifest import DOWNLOAD_BASE, version_key  # noqa: E402
from publish_update_manifest import (MANIFESTS, PAYLOAD_CACHE, PublicStore,  # noqa: E402
                                     PublishError, S3Store, file_digest, payload_assets)
from release_manual_signing import attached_after_signing  # noqa: E402

# The first release whose page is trimmed. v26.11.3 is the bridge and keeps
# its full page, and older pages are left as they were published.
OFFLOAD_FROM = "v26.11.4"

RELEASE_TAG = re.compile(r"^v[0-9]+\.[0-9]+\.[0-9]+(\.[0-9]+)?$")
# A throwaway release for testing the workflow end to end. Never a v* tag:
# that would start the build workflows, and with them Store publishing.
TEST_TAG = re.compile(r"^offload-test-[0-9]+$")
SAFE_NAME = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]*$")

BUILD_WORKFLOWS = ("AppImage", "Windows Installer", "macOS DMG")
SIGN_WORKFLOW = "Sign Release Artifacts"
SUMS = "SHA256SUMS.txt"
SUMS_SIG = SUMS + ".asc"
# A run that hasn't finished, in each state `gh run list --status` names.
UNFINISHED = ("queued", "in_progress", "waiting", "pending", "requested")

CONTENT_TYPES = {".asc": "application/pgp-signature",
                 ".xz": "application/x-xz",
                 ".msixupload": "application/octet-stream"}


class OffloadError(RuntimeError):
    pass


# ── Decisions (pure) ────────────────────────────────────────────────────────

def eligible(tag: str) -> bool:
    """A tag whose page this trims: a release from OFFLOAD_FROM on, or a test tag."""
    if TEST_TAG.match(tag):
        return True
    return bool(RELEASE_TAG.match(tag)) and version_key(tag) >= version_key(OFFLOAD_FROM)


def moves(tag: str, assets: list[dict]) -> list[dict]:
    """The assets that move off the page, by exact name for this tag."""
    prefix = f"AetherSDR-{tag}-"
    version = tag[1:] if tag.startswith("v") else tag
    msix = re.compile(rf"AetherSDR-{re.escape(version)}(\.[0-9]+)?-Windows-x64\.msixupload")
    out = []
    for a in assets:
        n = a["name"]
        if not SAFE_NAME.match(n):
            continue
        if n.endswith(".asc"):
            take = n.startswith(prefix) or n == f"AetherSDR-Manual-{tag}.pdf.asc"
        elif n.endswith("-symbols.tar.xz"):
            take = n.startswith(prefix)
        else:
            take = bool(msix.fullmatch(n))
        if take:
            out.append(a)
    return sorted(out, key=lambda a: a["name"])


def readiness(assets: list[dict], busy: list[str]) -> str | None:
    """None when the release is settled enough to move files off it, or why not."""
    if busy:
        return "still running: " + ", ".join(busy)
    by_name = {a["name"]: a for a in assets}
    if SUMS not in by_name or SUMS_SIG not in by_name:
        return "not signed yet: no SHA256SUMS.txt and SHA256SUMS.txt.asc"
    # The rule Docs-triggered signing uses to decide whether to sign again.
    newer = attached_after_signing(assets, by_name[SUMS])
    if newer:
        return "attached after the last signing: " + ", ".join(newer)
    return None


def named_by_manifests(tag: str, manifests: list[dict]) -> set[str]:
    """The release-page files the tag's published manifests download."""
    base = f"{DOWNLOAD_BASE}{tag}/"
    names = set()
    for m in manifests:
        for a, _ in payload_assets(m):
            if a["url"].startswith(base):
                names.add(a["url"][len(base):])
    return names


def key_for(tag: str, name: str) -> str:
    return f"releases/{tag}/{name}"


def content_type(name: str) -> str:
    for suffix, ctype in CONTENT_TYPES.items():
        if name.endswith(suffix):
            return ctype
    return "application/octet-stream"


# ── GitHub, through gh ──────────────────────────────────────────────────────

class GitHub:
    def __init__(self, repo: str) -> None:
        self.repo = repo

    def _gh(self, *args: str) -> str:
        out = subprocess.run(["gh", *args], capture_output=True, text=True)
        if out.returncode != 0:
            raise OffloadError(f"gh {' '.join(args[:3])}: {out.stderr.strip()}")
        return out.stdout

    def releases(self) -> list[dict]:
        return json.loads(self._gh("api", f"repos/{self.repo}/releases?per_page=20"))

    def assets(self, tag: str) -> list[dict]:
        rel = json.loads(self._gh("api", f"repos/{self.repo}/releases/tags/{tag}"))
        pages = self._gh("api", "--paginate", "--slurp",
                         f"repos/{self.repo}/releases/{rel['id']}/assets?per_page=100")
        return [{"id": a["id"], "name": a["name"], "size": a["size"],
                 "digest": a.get("digest"), "updatedAt": a["updated_at"]}
                for page in json.loads(pages) for a in page]

    def busy(self, tag: str) -> list[str]:
        """The runs that can still change this release's files."""
        out = []
        for wf in BUILD_WORKFLOWS:
            runs = json.loads(self._gh("run", "list", "-R", self.repo, "--workflow", wf,
                                       "--branch", tag, "--limit", "5", "--json", "status"))
            if any(r["status"] != "completed" for r in runs):
                out.append(wf)
        runs = json.loads(self._gh("run", "list", "-R", self.repo, "--workflow", "Docs",
                                   "--event", "release", "--branch", tag, "--limit", "5",
                                   "--json", "status"))
        if any(r["status"] != "completed" for r in runs):
            out.append("Docs")
        # A signing run's head branch is the default branch, whatever tag it
        # signs, so any unfinished one counts. Asked by state, not from the
        # newest few: every main build adds a skipped one.
        for status in UNFINISHED:
            runs = json.loads(self._gh("run", "list", "-R", self.repo, "--workflow", SIGN_WORKFLOW,
                                       "--status", status, "--limit", "1", "--json", "status"))
            if runs:
                out.append(f"{SIGN_WORKFLOW} ({status})")
                break
        return out

    def download(self, tag: str, name: str, into: Path) -> Path:
        self._gh("release", "download", tag, "-R", self.repo, "--pattern", name,
                 "--dir", str(into), "--clobber")
        return into / name

    def delete(self, asset_id: int) -> None:
        self._gh("api", "-X", "DELETE", f"repos/{self.repo}/releases/assets/{asset_id}")


# ── Plan and offload ────────────────────────────────────────────────────────

@dataclass
class Plan:
    tag: str | None
    ready: bool
    why: str
    files: list[dict]


def published_manifests(public, tag: str) -> list[dict]:
    out = []
    for name in MANIFESTS:
        data = public.get(key_for(tag, name))
        if data is not None:
            try:
                out.append(json.loads(data))
            except ValueError:
                raise OffloadError(f"releases/{tag}/{name} is not JSON") from None
    return out


def plan_tag(gh, public, tag: str) -> Plan:
    assets = gh.assets(tag)
    files = moves(tag, assets)
    if not files:
        return Plan(tag, False, "nothing to move", [])
    why = readiness(assets, gh.busy(tag))
    return Plan(tag, why is None, why or "ready", files)


def plan(gh, public, tag: str | None, dry_run: bool) -> Plan:
    if tag is not None:
        if not (RELEASE_TAG.match(tag) or TEST_TAG.match(tag)):
            raise OffloadError(f"{tag!r} is not a release tag")
        if not dry_run and not eligible(tag):
            raise OffloadError(f"{tag} is older than {OFFLOAD_FROM}, whose page is the first trimmed")
        return plan_tag(gh, public, tag)
    tags = [r["tag_name"] for r in gh.releases()
            if not r.get("draft") and RELEASE_TAG.match(r["tag_name"]) and eligible(r["tag_name"])]
    tags.sort(key=version_key, reverse=True)
    # The newest that is ready; if none is, the newest with files, to say why.
    waiting = None
    for t in tags:
        p = plan_tag(gh, public, t)
        if p.ready:
            return p
        if p.files and waiting is None:
            waiting = p
    if waiting is not None:
        return waiting
    return Plan(None, False, f"no release from {OFFLOAD_FROM} on has files to move", [])


def served_size(public, key: str) -> int | None:
    """The size updates.aethersdr.com serves for key, from the bucket rather
    than an edge copy: the query makes the edge's cache key one it hasn't
    seen. The bucket ignores it."""
    return public.size(f"{key}?offload-check={time.time_ns()}")


def offload(gh, s3, public, tag: str, work: Path, log=print) -> list[str]:
    """Copy and check every file, then delete them from the page. Returns
    the names moved. A failed copy or check deletes nothing; a run that
    stops partway through the deletes is repaired by running again."""
    p = plan(gh, public, tag, dry_run=False)
    if not p.files:
        log(f"{tag}: nothing to move")
        return []
    if not p.ready:
        raise OffloadError(f"{tag}: not ready: {p.why}")
    clash = named_by_manifests(tag, published_manifests(public, tag)) & {f["name"] for f in p.files}
    if clash:
        raise OffloadError("a published manifest downloads these from the page: " + ", ".join(sorted(clash)))
    work.mkdir(parents=True, exist_ok=True)
    copied: dict[str, dict] = {}
    for a in p.files:
        name = a["name"]
        path = gh.download(tag, name, work)
        sha, size = file_digest(path)
        if size != a["size"]:
            raise OffloadError(f"{name}: downloaded {size} bytes, the release lists {a['size']}")
        if a.get("digest") and a["digest"] != f"sha256:{sha}":
            raise OffloadError(f"{name}: downloaded SHA-256 {sha}, the release lists {a['digest']}")
        key = key_for(tag, name)
        s3.put_file(key, path, content_type(name), PAYLOAD_CACHE)
        stored = s3.digest(key)
        if stored != (sha, size):
            raise OffloadError(f"{key}: the bucket holds {stored}, not {(sha, size)}")
        if served_size(public, key) != size:
            raise OffloadError(f"{key}: updates.aethersdr.com does not serve the {size} bytes uploaded")
        path.unlink()
        copied[name] = a
        log(f"copied   {name}  {size} bytes  sha256 {sha}")
    # The release must still hold exactly what was copied, and nothing may be
    # about to change it: a signing run that replaced a signature, or is
    # running now, means the copy is stale.
    busy = gh.busy(tag)
    if busy:
        raise OffloadError("started while copying: " + ", ".join(busy) + "; nothing deleted")
    now = {a["name"]: a for a in gh.assets(tag)}
    for name, a in copied.items():
        b = now.get(name)
        if b is None or (b["id"], b["size"], b.get("digest"), b["updatedAt"]) != \
                (a["id"], a["size"], a.get("digest"), a["updatedAt"]):
            raise OffloadError(f"{name} changed on the release while it was copied; nothing deleted")
    for name, a in copied.items():
        try:
            gh.delete(a["id"])
        except OffloadError as e:
            raise OffloadError(f"deleting {name}: {e}. Every file is on updates.aethersdr.com; "
                               "run again to finish trimming the page") from None
        log(f"deleted  {name} from the release page")
    left = {a["name"] for a in gh.assets(tag)} & set(copied)
    if left:
        raise OffloadError("still on the release page: " + ", ".join(sorted(left)))
    return sorted(copied)


# ── CLI ─────────────────────────────────────────────────────────────────────

def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("plan")
    p.add_argument("--tag")
    p.add_argument("--dry-run", action="store_true")
    p.add_argument("--github-output", type=Path)
    o = sub.add_parser("offload")
    o.add_argument("--tag", required=True)
    o.add_argument("--work", type=Path, required=True)
    args = ap.parse_args(argv)
    try:
        repo = os.environ["GH_REPO"]
    except KeyError:
        print("error: GH_REPO is not set", file=sys.stderr)
        return 2
    gh, public = GitHub(repo), PublicStore()
    try:
        if args.cmd == "plan":
            result = plan(gh, public, args.tag, args.dry_run)
            print(f"{result.tag or '-'}: {result.why}")
            for a in result.files:
                state = ""
                if args.dry_run:
                    on_r2 = served_size(public, key_for(result.tag, a["name"])) == a["size"]
                    state = "  (on R2)" if on_r2 else "  (not on R2 yet)"
                print(f"  move  {a['name']}  {a['size']} bytes{state}")
            if args.github_output:
                ready = result.ready and not args.dry_run
                with args.github_output.open("a", encoding="utf-8") as f:
                    f.write(f"tag={result.tag or ''}\nready={'true' if ready else 'false'}\n")
            return 0
        moved = offload(gh, S3Store(), public, args.tag, args.work)
        print(f"{args.tag}: moved {len(moved)} files to updates.aethersdr.com/releases/{args.tag}/")
        return 0
    except (OffloadError, PublishError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
