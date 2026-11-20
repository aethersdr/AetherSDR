#!/usr/bin/env python3
"""check_release_assets.py — the post-tag verification list as one command.

Steps 3, 4 and 5 of the tag-release skill: the tag and its verification, the
release's flags and title, the three build runs and the signing runs, the
asset set with signature timing and the ``.msixupload`` version (from
v26.11.4, the 14-file page and the files Release Offload moved to
updates.aethersdr.com), the
``SHA256SUMS.txt`` coverage, downloaded-and-verified signatures and checksums,
the source tarball's version, and the Store staging step. Every check prints
``PASS``, ``FAIL``, ``WARN``, ``INFO`` or ``SKIP``; the exit status is
non-zero when any check FAILs.

Standard library plus ``gh`` and ``git``; ``gpg`` when present (the signature
checks are SKIPped, never passed, without it). Runs on Windows, macOS and
Linux. Run it from anywhere inside an AetherSDR clone, or pass ``--key``.

Usage::

    python check_release_assets.py --version v26.9.5 [--download-dir DIR] [--all]
                                   [--not-latest] [--key docs/RELEASE-SIGNING-KEY.pub.asc]
                                   [--skip-download] [--repo aethersdr/AetherSDR]
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Any

KEY_FPR = "B7656E6BCB2E022B79F0F97B5578D10E3D5918F3"
SIGNABLE = ("x86_64.AppImage", "aarch64.AppImage", "x86_64.tar.zst", "aarch64.tar.zst",
            "macOS-apple-silicon.app.tar.zst", "macOS-intel.app.tar.zst",
            "Windows-x64-setup.exe", "Windows-x64-portable.zip")
LINUX_TARBALLS = ("x86_64.tar.zst", "aarch64.tar.zst")
APP_TARBALLS = ("macOS-apple-silicon.app.tar.zst", "macOS-intel.app.tar.zst")
BUILD_WORKFLOWS = ("AppImage", "Windows Installer", "macOS DMG")


def signables(tarballs: bool, app_tarballs: bool = True) -> tuple[str, ...]:
    """SIGNABLE for a release: the Linux tarballs only once its AppImage build made
    them, the macOS app tarballs only once its macOS DMG build made them."""
    return tuple(s for s in SIGNABLE
                 if (tarballs or s not in LINUX_TARBALLS) and (app_tarballs or s not in APP_TARBALLS))
# Started by publishing the release, not by the tag push: it typesets and
# attaches the PDF manual (and, on releases before RFC #6964, signs it).
DOCS_WORKFLOW = "Docs"
STORE_STEP = "Publish Microsoft Store package"
# Where Release Offload moves the files people don't download (RFC #6964).
R2_RELEASES = "https://updates.aethersdr.com/releases/"

results: list[tuple[str, str, str]] = []


# --------------------------------------------------------------------------- io

def _utf8_stdio() -> None:
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")  # type: ignore[attr-defined]
        except (AttributeError, ValueError):
            pass  # not a TextIOWrapper (a captured or redirected stream); leave its encoding alone


def report(status: str, name: str, detail: str = "") -> None:
    results.append((status, name, detail))
    line = f"{status:<4} {name}"
    if detail:
        line += f"\n     {detail.replace(chr(10), chr(10) + '     ')}"
    print(line, flush=True)


def _tool(name: str) -> str:
    path = shutil.which(name)
    if not path:
        sys.exit(f"error: `{name}` is not on PATH")
    return path


def run(argv: list[str], *, check: bool = True) -> subprocess.CompletedProcess[str]:
    proc = subprocess.run(argv, capture_output=True, text=True, encoding="utf-8", errors="replace")
    if check and proc.returncode != 0:
        sys.exit(f"error: {' '.join(argv)}\n{proc.stderr.strip()}")
    return proc


def gh_json(*args: str, check: bool = True) -> Any:
    proc = run([_tool("gh"), *args], check=check)
    if proc.returncode != 0:
        return None
    return json.loads(proc.stdout) if proc.stdout.strip() else None


def ts(s: str | None) -> datetime | None:
    if not s:
        return None
    return datetime.fromisoformat(s.replace("Z", "+00:00"))


def fmt(d: datetime | None) -> str:
    return d.strftime("%Y-%m-%dT%H:%M:%SZ") if d else "?"


# ------------------------------------------------------------------------ tag

def check_tag(repo: str, v: str) -> tuple[str | None, str | None, datetime | None]:
    ref = gh_json("api", f"repos/{repo}/git/ref/tags/{v}", check=False)
    if not ref:
        report("FAIL", f"tag {v} exists on origin")
        return None, None, None
    obj = ref["object"]
    if obj["type"] != "tag":
        report("FAIL", f"tag {v} is an annotated tag object", f"it is a lightweight tag on {obj['sha'][:9]}")
        return obj["sha"], None, None
    tag = gh_json("api", f"repos/{repo}/git/tags/{obj['sha']}")
    commit = tag["object"]["sha"]
    ver = tag.get("verification") or {}
    tagger = tag.get("tagger") or {}
    tagged_at = ts(tagger.get("date"))
    first = (tag.get("message") or "").split("\n", 1)[0].strip()
    report("PASS", f"tag {v} is annotated", f"commit {commit[:9]}, tag object {obj['sha'][:9]}, tagger {tagger.get('name')} at {fmt(tagged_at)}")
    if ver.get("verified") and ver.get("reason") == "valid":
        report("PASS", "tag signature verified by GitHub", f"reason={ver.get('reason')}")
    else:
        report("FAIL", "tag signature verified by GitHub", f"verified={ver.get('verified')} reason={ver.get('reason')}")
    if shutil.which("git"):
        run([_tool("git"), "fetch", "--quiet", "origin", f"refs/tags/{v}:refs/tags/{v}", "origin/main"], check=False)
        p = run([_tool("git"), "merge-base", "--is-ancestor", commit, "origin/main"], check=False)
        if p.returncode == 0:
            report("PASS", "tag commit is an ancestor of origin/main")
        else:
            report("FAIL", "tag commit is an ancestor of origin/main", f"{commit[:9]} is not reachable from origin/main")
    else:
        report("SKIP", "tag commit is an ancestor of origin/main", "git not on PATH")
    return commit, first, tagged_at


# -------------------------------------------------------------------- release

def check_release(repo: str, v: str, tag_first_line: str | None, not_latest: bool) -> dict[str, Any] | None:
    rel = gh_json("api", f"repos/{repo}/releases/tags/{v}", check=False)
    if not rel:
        report("FAIL", f"release {v} exists")
        return None
    flags = []
    ok = True
    if rel.get("draft"):
        flags.append("draft"); ok = False
    if rel.get("prerelease"):
        flags.append("pre-release"); ok = False
    if rel.get("target_commitish") != "main":
        flags.append(f"target_commitish={rel.get('target_commitish')}"); ok = False
    report("PASS" if ok else "FAIL", "release is not draft, not pre-release, target main",
           ", ".join(flags) or f"author {rel['author']['login']}, created {rel['created_at']}, published {rel['published_at']}")
    if rel["author"]["login"] == "github-actions[bot]":
        report("WARN", "release was created by CI, not by the cutter", "the first asset upload created a bare release; title and body were pasted in afterwards")
    if tag_first_line is not None:
        if rel.get("name") == tag_first_line:
            report("PASS", "release title equals the tag's first line", rel["name"])
        else:
            report("FAIL", "release title equals the tag's first line", f"title: {rel.get('name')!r}\ntag:   {tag_first_line!r}")
    latest = gh_json("api", f"repos/{repo}/releases/latest", check=False) or {}
    if not_latest:
        report("INFO", "latest release", f"{latest.get('tag_name')} (--not-latest: this release is expected not to be Latest)")
    elif latest.get("tag_name") == v:
        report("PASS", "releases/latest names this tag")
    else:
        report("FAIL", "releases/latest names this tag", f"latest is {latest.get('tag_name')}")
    return rel


# ----------------------------------------------------------------------- runs

def check_runs(repo: str, v: str, tagged_at: datetime | None) -> tuple[dict[str, Any] | None, list[dict[str, Any]]]:
    runs = gh_json("run", "list", "--repo", repo, "--branch", v, "--limit", "30", "--json",
                   "workflowName,event,status,conclusion,attempt,createdAt,updatedAt,url,databaseId") or []
    by_wf: dict[str, dict[str, Any]] = {}
    for r in sorted(runs, key=lambda r: r["createdAt"]):
        by_wf[r["workflowName"]] = r  # newest run per workflow
    windows_run = None
    for wf in BUILD_WORKFLOWS:
        r = by_wf.get(wf)
        if not r:
            report("FAIL", f"{wf} run on the tag", "no run found")
            continue
        detail = f"attempt {r['attempt']}, {r['status']}, {r['createdAt']} → {r['updatedAt']}\n{r['url']}"
        if r["status"] != "completed":
            report("WARN", f"{wf} run on the tag", f"still {r['status']}\n{detail}")
        elif r["conclusion"] == "success":
            report("PASS", f"{wf} run on the tag", detail)
        elif wf == "Windows Installer":
            report("INFO", f"{wf} run on the tag concluded {r['conclusion']}", f"read the step results below before calling it a build failure\n{detail}")
        else:
            report("FAIL", f"{wf} run on the tag", f"conclusion {r['conclusion']}\n{detail}")
        if wf == "Windows Installer":
            windows_run = r
        if wf == "macOS DMG":
            _check_macos_steps(repo, r)
    docs = by_wf.get(DOCS_WORKFLOW)
    if not docs:
        report("WARN", f"{DOCS_WORKFLOW} run for the release", "no run found on the tag; the manual's assets below say whether it ran")
    else:
        detail = f"attempt {docs['attempt']}, {docs['event']}, {docs['status']}, {docs['createdAt']} → {docs['updatedAt']}\n{docs['url']}"
        if docs["status"] != "completed":
            report("WARN", f"{DOCS_WORKFLOW} run for the release", f"still {docs['status']}\n{detail}")
        elif docs["conclusion"] == "success":
            report("PASS", f"{DOCS_WORKFLOW} run for the release", detail)
        else:
            report("FAIL", f"{DOCS_WORKFLOW} run for the release", f"conclusion {docs['conclusion']}\n{detail}")
    extra = sorted({r["workflowName"] for r in runs} - set(BUILD_WORKFLOWS) - {DOCS_WORKFLOW})
    if extra:
        report("WARN", "other workflows ran on the tag", ", ".join(extra))
    # signing runs execute on the default branch; match by time. The window
    # opens at the tag object's tagger date (the skill tags and pushes in the
    # same breath); a tag created long before it was pushed would need the push
    # time instead, which the API does not record.
    sign = gh_json("run", "list", "--repo", repo, "--workflow", "sign-release.yml", "--limit", "12", "--json",
                   "event,status,conclusion,createdAt,updatedAt,url,databaseId") or []
    since = tagged_at - timedelta(minutes=5) if tagged_at else None
    mine = [s for s in sign if since is None or (ts(s["createdAt"]) or datetime.min.replace(tzinfo=timezone.utc)) >= since]
    if not mine:
        report("FAIL", "a Sign Release Artifacts run started after the tag", "none found; dispatch: gh workflow run sign-release.yml -f tag=" + v)
    else:
        lines = [f"{s['createdAt']} {s['event']} {s['status']} {s['conclusion']} {s['url']}" for s in mine]
        if any(s["conclusion"] == "success" for s in mine):
            report("PASS", "a Sign Release Artifacts run succeeded after the tag", "\n".join(lines))
        elif any(s["status"] != "completed" for s in mine):
            report("WARN", "Sign Release Artifacts still running", "\n".join(lines))
        else:
            report("FAIL", "a Sign Release Artifacts run succeeded after the tag", "\n".join(lines))
    return windows_run, mine


def _check_macos_steps(repo: str, r: dict[str, Any]) -> None:
    view = gh_json("run", "view", "--repo", repo, str(r["databaseId"]), "--json", "jobs") or {}
    for job in view.get("jobs", []):
        steps = {s["name"]: s["conclusion"] for s in job.get("steps", [])}
        # The build legs sign nothing: only the signing and app-tarball jobs are read.
        if job["name"].startswith("Sign and notarize"):
            what = "macOS notarization steps"
            names = ("Code sign application", "Sign DMG", "Notarize DMG", "Attach to release")
        elif job["name"].startswith("App update tarball"):
            what = "macOS app tarball steps"
            names = ("Take the app out", "Build the app tarball", "Attach to release")
        else:
            continue
        wanted = {k: steps.get(k) for k in names}
        verdict = "PASS" if all(c == "success" for c in wanted.values()) else "FAIL"
        report(verdict, f"{what}: {job['name']}", ", ".join(f"{k} {c}" for k, c in wanted.items()))


# --------------------------------------------------------------------- assets

def release_makes_tarballs(repo: str, v: str) -> bool:
    """Whether the tag's own AppImage workflow builds the Linux release
    tarballs: a release cut before that carries none, and is not short of them."""
    proc = run([_tool("gh"), "api", f"repos/{repo}/contents/.github/workflows/appimage.yml?ref={v}",
                "-H", "Accept: application/vnd.github.raw"], check=False)
    if proc.returncode != 0:
        report("WARN", "the tag's AppImage workflow read", "could not read it; expecting the Linux tarballs")
        return True
    return "--tarball" in proc.stdout


def release_makes_app_tarballs(repo: str, v: str) -> bool:
    """Whether the tag's own macOS DMG workflow builds the app tarballs (the
    in-app update's macOS payload): a release cut before that carries none."""
    proc = run([_tool("gh"), "api", f"repos/{repo}/contents/.github/workflows/macos-dmg.yml?ref={v}",
                "-H", "Accept: application/vnd.github.raw"], check=False)
    if proc.returncode != 0:
        report("WARN", "the tag's macOS DMG workflow read", "could not read it; expecting the macOS app tarballs")
        return True
    return "--app-tarball" in proc.stdout


def _tag_file(repo: str, v: str, path: str) -> tuple[bool, str | None]:
    """(read ok, text) of one file at the tag. A 404 is a successful read of
    an absent file: (True, None)."""
    proc = run([_tool("gh"), "api", f"repos/{repo}/contents/{path}?ref={v}",
                "-H", "Accept: application/vnd.github.raw"], check=False)
    if proc.returncode == 0:
        return True, proc.stdout
    return ("Not Found" in (proc.stderr + proc.stdout) or "404" in proc.stderr), None


def _repo_tools() -> None:
    path = str(Path(__file__).resolve().parents[4] / "tools")
    if path not in sys.path:
        sys.path.insert(0, path)


def release_offloaded(v: str) -> bool:
    """Whether Release Offload trims this release's page (from v26.11.4).
    tools/offload_release_assets.py's rule, so the two always agree."""
    return _offload().eligible(v)


def moved_off_page(v: str, names: list[str]) -> list[str]:
    """Of `names`, the ones Release Offload moves to R2, by its own rule."""
    return [a["name"] for a in _offload().moves(v, [{"name": n} for n in names])]


# r2_size's answer when updates.aethersdr.com could not be asked.
UNKNOWN = -1


def _offload():
    _repo_tools()
    import offload_release_assets  # noqa: PLC0415
    return offload_release_assets


def r2_size(v: str, name: str) -> int | None:
    """The size updates.aethersdr.com serves for a release file, None when it
    is absent, or UNKNOWN when it could not be asked. Read past the edge
    cache, the way Release Offload checks its copies."""
    o = _offload()
    try:
        size = o.served_size(o.PublicStore(), o.key_for(v, name))
    except o.PublishError:
        return UNKNOWN
    return size


def r2_fetch(v: str, name: str, dest: Path) -> bool:
    o = _offload()
    try:
        data = o.PublicStore().get(f"{o.key_for(v, name)}?check={time.time_ns()}")
    except o.PublishError as e:
        report("FAIL", f"download {R2_RELEASES}{v}/{name}", str(e))
        return False
    if data is None:
        report("FAIL", f"download {R2_RELEASES}{v}/{name}", "HTTP 404")
        return False
    dest.write_bytes(data)
    return True


def release_sums_manual(repo: str, v: str) -> bool:
    """Whether the manual is listed in SHA256SUMS.txt with no .asc of its own
    (RFC #6964), or signed apart as before. The rule is
    tools/release_manual_signing.py's, applied to the tag's own files, the
    same call sign-release.yml makes, so the two always agree."""
    _repo_tools()
    import release_manual_signing  # noqa: PLC0415
    reads = {path: _tag_file(repo, v, path)
             for path in (".github/workflows/docs.yml", "tools/docs/build_pdf.py", "docs/user/sidebars.js")}
    if not all(ok for ok, _ in reads.values()):
        report("FAIL", "the tag's manual shape", "could not read the tag's docs files; the expected set below may be wrong")
        return True
    has_site = reads["tools/docs/build_pdf.py"][1] is not None and reads["docs/user/sidebars.js"][1] is not None
    return release_manual_signing.shape_of(reads[".github/workflows/docs.yml"][1], has_site) == "sums"


def expected_assets(v: str, hotfix: bool, tarballs: bool = True, app_tarballs: bool = True,
                    manual_in_sums: bool = True) -> list[str]:
    bare = ".".join(v.lstrip("v").split(".")[:3])  # 26.9.5 and 26.9.5.0 both package as 26.9.5.0
    names = [f"AetherSDR-{v}-x86_64.AppImage", f"AetherSDR-{v}-aarch64.AppImage",
             *([f"AetherSDR-{v}-x86_64.tar.zst", f"AetherSDR-{v}-aarch64.tar.zst"] if tarballs else []),
             *([f"AetherSDR-{v}-{s}" for s in APP_TARBALLS] if app_tarballs else []),
             f"AetherSDR-{v}-macOS-apple-silicon.dmg", f"AetherSDR-{v}-macOS-intel.dmg",
             f"AetherSDR-{v}-Windows-x64-setup.exe", f"AetherSDR-{v}-Windows-x64-portable.zip",
             f"AetherSDR-{v}-source.tar.gz", "SHA256SUMS.txt", f"AetherSDR-Manual-{v}.pdf"]
    manual = f"AetherSDR-Manual-{v}.pdf"
    signed = [n + ".asc" for n in names if not n.endswith(".dmg") and not (manual_in_sums and n == manual)]
    out = names + signed
    if not hotfix:
        out.append(f"AetherSDR-{bare}.0-Windows-x64.msixupload")
    return out


def expected_symbol_assets(v: str) -> list[str]:
    # Debug-symbol archives, one per platform build (docs/debugging-crashes.md).
    # Kept out of the asset set: users never download them, nothing signs them,
    # and a missing one costs crash symbolization, not the release.
    return [f"AetherSDR-{v}-x86_64-symbols.tar.xz", f"AetherSDR-{v}-aarch64-symbols.tar.xz",
            f"AetherSDR-{v}-macOS-apple-silicon-symbols.tar.xz", f"AetherSDR-{v}-macOS-intel-symbols.tar.xz",
            f"AetherSDR-{v}-Windows-x64-symbols.tar.xz"]


def is_patch_bundle(v: str, name: str) -> bool:
    return name.startswith(f"AetherSDR-{v}-") and re.search(r"-from-v[0-9.]+\.update\.tar$", name) is not None


def check_assets(rel: dict[str, Any], v: str, hotfix: bool, tarballs: bool,
                 app_tarballs: bool = True, manual_in_sums: bool = True,
                 offloaded: bool = False) -> tuple[dict[str, dict[str, Any]], list[str]]:
    """Checks the page, and from v26.11.4 the files Release Offload moved to
    updates.aethersdr.com. Returns the page's assets and the names served
    there instead of on the page."""
    assets = {a["name"]: a for a in rel.get("assets", [])}
    want = expected_assets(v, hotfix, tarballs, app_tarballs, manual_in_sums)
    symbols = expected_symbol_assets(v)
    on_r2: list[str] = []
    page = want
    if offloaded:
        moved = set(moved_off_page(v, want))
        page = [n for n in want if n not in moved]
        moving = [n for n in want if n in moved] + symbols
        # A hotfix makes no .msixupload; one moved off its page is still one.
        if hotfix:
            moving.append(f"AetherSDR-{v.lstrip('v')}-Windows-x64.msixupload")
        with ThreadPoolExecutor(max_workers=8) as pool:
            sizes = dict(zip(moving, pool.map(lambda n: r2_size(v, n), moving)))
        still, absent, unknown = [], [], []
        for n in moving:
            size = sizes[n]
            if size == UNKNOWN:
                (still if n in assets else unknown).append(n)
            elif size is not None and size > 0 and (n not in assets or size == assets[n]["size"]):
                on_r2.append(n)
                if n in assets:
                    still.append(n)
            elif n in assets:
                still.append(n)
            elif size is not None:
                absent.append(f"{n} ({size} bytes)")
            else:
                absent.append(n)
        if hotfix:
            stray = moving.pop()
            # On the page, the check below reports it.
            if stray in on_r2 and stray not in assets:
                report("FAIL", ".msixupload absent for a hotfix", f"{stray}, on {R2_RELEASES}{v}/")
            for lst in (still, absent, unknown, on_r2):
                if stray in lst:
                    lst.remove(stray)
        if unknown:
            report("WARN", f"updates.aethersdr.com answered for every moved file",
                   "could not check: " + ", ".join(unknown))
        lost = [n for n in absent if n.split(" ")[0] not in symbols]
        if lost:
            report("FAIL", f"the {len(moving) - len(symbols)} signatures and packages Release Offload moves are "
                   "on updates.aethersdr.com or the page", "on neither: " + ", ".join(lost))
        sym_lost = [n for n in absent if n.split(" ")[0] in symbols]
        if sym_lost:
            report("WARN", f"the {len(symbols)} debug-symbol archives are present",
                   "on neither updates.aethersdr.com nor the page: " + ", ".join(sym_lost)
                   + " (crashes on that build cannot be symbolized)")
        if still:
            report("WARN", "Release Offload has moved its files off the page",
                   "still on the page: " + ", ".join(still)
                   + "\napprove its update-signing run, or dispatch release-offload.yml with dry_run=false")
        elif not absent and not unknown:
            report("PASS", f"the {len(moving)} files Release Offload moves are on updates.aethersdr.com",
                   f"{R2_RELEASES}{v}/")
    missing = [n for n in page if n not in assets]
    n_want = len(page)
    label = f"the {n_want}-asset page" if offloaded else f"the {n_want}-asset set"
    if missing:
        report("FAIL", f"{label} is present", "missing: " + ", ".join(missing))
    else:
        where = "hotfix: no .msixupload" if hotfix else (
            "the .msixupload is checked on updates.aethersdr.com" if offloaded else "incl. .msixupload")
        report("PASS", f"{label} is present ({where})")
    if not offloaded:
        sym_missing = [n for n in symbols if n not in assets]
        if sym_missing:
            report("WARN", f"the {len(symbols)} debug-symbol archives are present",
                   "missing: " + ", ".join(sym_missing) + " (crashes on that build cannot be symbolized)")
        else:
            report("PASS", f"the {len(symbols)} debug-symbol archives are present")
    bundles = sorted(n for n in assets if is_patch_bundle(v, n))
    if bundles and offloaded:
        report("WARN", "no patch bundles on the page",
               f"{len(bundles)} on the page: Update Manifest ran with UPDATE_PAYLOAD_BASE unset or github, "
               "so manifest-v2.json names them here and Release Offload leaves them")
    elif bundles:
        report("INFO", "patch bundles on the page", f"{len(bundles)} (Update Manifest attaches them)")
    extra = sorted(set(assets) - set(want) - set(symbols) - set(bundles))
    if extra:
        bad = [e for e in extra if "msixupload" in e]
        report("FAIL" if bad else "WARN", "no unexpected assets", ", ".join(extra))
    # The six CI-built binaries are attached by other workflows before the
    # signing job runs, so their .asc must be strictly newer. The source tarball and
    # SHA256SUMS.txt (the signing job), and the manual PDF on releases that
    # sign it apart (the Docs workflow), are uploaded in the same
    # `gh release upload` as their signatures, so their timestamps tie (either
    # order, within a few seconds); only a real gap there means a re-sign
    # clobbered one file and not the other.
    built = [f"AetherSDR-{v}-{s}" for s in signables(tarballs, app_tarballs)]
    late, gaps = [], []
    for n in want:
        # A signature on updates.aethersdr.com has no page timestamp; gpg
        # checks it below instead.
        if not n.endswith(".asc") or n not in assets or n[:-4] not in assets:
            continue
        d = ts(assets[n]["created_at"]) - ts(assets[n[:-4]]["created_at"])
        secs = d.total_seconds()
        if n[:-4] in built:
            if secs <= 0:
                late.append(f"{n} {assets[n]['created_at']} <= {n[:-4]} {assets[n[:-4]]['created_at']}")
            else:
                gaps.append(f"{n[:-4]}: +{int(secs // 60)} min")
        elif abs(secs) > 30:
            late.append(f"{n} and {n[:-4]} were not uploaded together ({int(secs)} s apart)")
        else:
            gaps.append(f"{n[:-4]}: same upload as its .asc")
    if offloaded and "SHA256SUMS.txt" in assets:
        # The signatures on updates.aethersdr.com came from the signing run
        # that wrote SHA256SUMS.txt, so a binary re-attached after it is
        # signed by none of them.
        sums_at = ts(assets["SHA256SUMS.txt"]["created_at"])
        after = [n for n in built if n in assets and ts(assets[n]["created_at"]) >= sums_at]
        if after:
            report("FAIL", "every signed binary predates SHA256SUMS.txt (its signatures are on updates.aethersdr.com)",
                   "attached after signing: " + ", ".join(after))
        else:
            report("PASS", "every signed binary predates SHA256SUMS.txt (its signatures are on updates.aethersdr.com)")
    if late:
        report("FAIL", "every .asc postdates the binary it signs (tarball and SHA256SUMS travel with theirs)", "\n".join(late))
    elif gaps:
        report("PASS", "every .asc postdates the binary it signs (tarball and SHA256SUMS travel with theirs)", "\n".join(gaps))
    for n in assets:
        if n.endswith(".msixupload"):
            m = re.match(r"AetherSDR-(\d+\.\d+\.\d+\.\d+)-Windows-x64\.msixupload$", n)
            if hotfix:
                report("FAIL", ".msixupload absent for a hotfix", n)
            elif m and m.group(1) == v.lstrip("v") + ".0":
                report("PASS", ".msixupload version is the release version plus .0", m.group(1))
            else:
                report("FAIL", ".msixupload version is the release version plus .0", n)
    for msix in [n for n in want if n.endswith(".msixupload") and n in on_r2 and n not in assets]:
        report("PASS", ".msixupload version is the release version plus .0",
               msix[len("AetherSDR-"):-len("-Windows-x64.msixupload")] + " (on updates.aethersdr.com)")
    uploaders = sorted({a["uploader"]["login"] for a in assets.values()})
    if uploaders != ["github-actions[bot]"]:
        report("WARN", "every asset was uploaded by CI", "uploaders: " + ", ".join(uploaders))
    return assets, on_r2


# ------------------------------------------------------------------- downloads

def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def check_downloads(repo: str, v: str, assets: dict[str, Any], args: argparse.Namespace, tarballs: bool,
                    app_tarballs: bool = True, manual_in_sums: bool = True,
                    on_r2: list[str] | None = None) -> None:
    if args.skip_download:
        report("SKIP", "download and verify signatures and checksums", "--skip-download")
        return
    ddir = Path(args.download_dir or tempfile.mkdtemp(prefix=f"aethersdr-{v}-"))
    ddir.mkdir(parents=True, exist_ok=True)
    patterns = ["SHA256SUMS.txt", "SHA256SUMS.txt.asc", f"AetherSDR-{v}-source.tar.gz", f"AetherSDR-{v}-source.tar.gz.asc",
                f"AetherSDR-{v}-x86_64.AppImage", f"AetherSDR-{v}-x86_64.AppImage.asc",
                f"AetherSDR-Manual-{v}.pdf", *([] if manual_in_sums else [f"AetherSDR-Manual-{v}.pdf.asc"])]
    if tarballs:
        # The in-app full download for Linux: its bytes and signature too.
        patterns += [f"AetherSDR-{v}-x86_64.tar.zst", f"AetherSDR-{v}-x86_64.tar.zst.asc"]
    if app_tarballs:
        # And macOS's (Apple silicon), the app the in-app update installs.
        patterns += [f"AetherSDR-{v}-macOS-apple-silicon.app.tar.zst",
                     f"AetherSDR-{v}-macOS-apple-silicon.app.tar.zst.asc"]
    on_r2 = on_r2 or []
    if args.all:
        patterns = [n for n in [*assets, *on_r2] if not n.endswith((".dmg", ".msixupload", "-symbols.tar.xz"))
                    and not is_patch_bundle(v, n)]
    # From v26.11.4 the per-file signatures come from updates.aethersdr.com.
    remote = [p for p in patterns if p not in assets and p in on_r2]
    patterns = [p for p in patterns if p in assets]
    argv = ["release", "download", v, "--repo", repo, "--dir", str(ddir), "--clobber"]
    for p in patterns:
        argv += ["--pattern", p]
    proc = run([_tool("gh"), *argv], check=False)
    got = [p for p in patterns if (ddir / p).exists()]
    if proc.returncode != 0 or len(got) != len(patterns):
        report("FAIL", "download the verification set", (proc.stderr or "").strip() or f"got {len(got)}/{len(patterns)}")
        return
    fetched = [p for p in remote if r2_fetch(v, p, ddir / p)]
    report("INFO", "downloaded", f"{len(got)} files from the release page"
           + (f", {len(fetched)} from {R2_RELEASES}{v}/" if remote else "") + f" into {ddir}")

    sums_path = ddir / "SHA256SUMS.txt"
    if sums_path.exists():
        entries = {}
        for line in sums_path.read_text(encoding="utf-8").splitlines():
            m = re.match(r"^([0-9a-f]{64})\s+\*?(.+)$", line.strip())
            if m:
                entries[m.group(2)] = m.group(1)
        want = [f"AetherSDR-{v}-{s}" for s in signables(tarballs, app_tarballs)] + [f"AetherSDR-{v}-source.tar.gz"]
        if manual_in_sums:
            want.append(f"AetherSDR-Manual-{v}.pdf")
        covers = f"SHA256SUMS.txt covers exactly the {len(want)} {'files (signables, source, manual)' if manual_in_sums else 'signable files'}"
        if sorted(entries) == sorted(want):
            report("PASS", covers)
        else:
            report("FAIL", covers,
                   f"listed: {', '.join(sorted(entries)) or '(none)'}\nexpected: {', '.join(want)}")
        bad, checked = [], []
        for name, digest in entries.items():
            p = ddir / name
            if p.exists():
                actual = sha256(p)
                (checked if actual == digest else bad).append(name)
        if bad:
            report("FAIL", "sha256 of downloaded files matches SHA256SUMS.txt", "mismatch: " + ", ".join(bad))
        elif checked:
            report("PASS", "sha256 of downloaded files matches SHA256SUMS.txt", ", ".join(checked))
        else:
            report("SKIP", "sha256 of downloaded files matches SHA256SUMS.txt", "no listed file was downloaded")
    else:
        report("FAIL", "SHA256SUMS.txt downloaded")

    gpg = shutil.which("gpg")
    if not gpg:
        report("SKIP", "gpg --verify SHA256SUMS.txt.asc", "gpg is not on PATH (Gpg4win on Windows); signature NOT verified")
        report("SKIP", "gpg --verify one artifact signature", "gpg is not on PATH; signature NOT verified")
    else:
        key = Path(args.key) if args.key else _find_key()
        if key and key.exists():
            p = run([gpg, "--batch", "--import", str(key)], check=False)
            fp = run([gpg, "--batch", "--with-colons", "--fingerprint", KEY_FPR], check=False)
            if KEY_FPR in fp.stdout.replace(" ", ""):
                report("PASS", "release signing key imported", f"{key} → {KEY_FPR}")
            else:
                report("FAIL", "release signing key imported", (p.stderr or "").strip())
        else:
            report("WARN", "release signing key file", "docs/RELEASE-SIGNING-KEY.pub.asc not found; relying on the local keyring")
        for name in ["SHA256SUMS.txt", f"AetherSDR-{v}-source.tar.gz", f"AetherSDR-{v}-x86_64.AppImage",
                     *([f"AetherSDR-{v}-x86_64.tar.zst"] if tarballs else []),
                     *([f"AetherSDR-{v}-macOS-apple-silicon.app.tar.zst"] if app_tarballs else []),
                     # With the manual in SHA256SUMS.txt, the sha256 check above
                     # plus SHA256SUMS.txt.asc is its verification.
                     *([] if manual_in_sums else [f"AetherSDR-Manual-{v}.pdf"])]:
            sig = ddir / (name + ".asc")
            if not sig.exists() or not (ddir / name).exists():
                report("SKIP", f"gpg --verify {name}.asc", "not downloaded")
                continue
            p = run([gpg, "--batch", "--status-fd", "1", "--verify", str(sig), str(ddir / name)], check=False)
            good = re.search(r"\[GNUPG:\] VALIDSIG ([0-9A-F]{40})", p.stdout)
            if good and good.group(1) == KEY_FPR:
                report("PASS", f"gpg --verify {name}.asc", f"VALIDSIG {KEY_FPR}")
            elif good:
                report("FAIL", f"gpg --verify {name}.asc", f"signed by {good.group(1)}, not the release key")
            else:
                report("FAIL", f"gpg --verify {name}.asc", (p.stderr or p.stdout).strip()[-400:])

    tarball = ddir / f"AetherSDR-{v}-source.tar.gz"
    if tarball.exists():
        try:
            with tarfile.open(tarball, "r:gz") as tf:
                member = tf.extractfile(f"AetherSDR-{v}/CMakeLists.txt")
                text = member.read().decode("utf-8", "replace") if member else ""
            m = re.search(r"project\(AetherSDR\s+VERSION\s+([0-9.]+)", text)
            if m and m.group(1) == v.lstrip("v"):
                report("PASS", "source tarball CMakeLists.txt says the release version", m.group(1))
            else:
                report("FAIL", "source tarball CMakeLists.txt says the release version", f"found {m.group(1) if m else 'nothing'} (tarball archived from main, not the tag?)")
        except (tarfile.TarError, KeyError, OSError) as e:
            report("FAIL", "source tarball opens and contains CMakeLists.txt", str(e))


def _find_key() -> Path | None:
    git = shutil.which("git")
    p = run([git, "rev-parse", "--show-toplevel"], check=False) if git else None
    if p is not None and p.returncode == 0:
        cand = Path(p.stdout.strip()) / "docs" / "RELEASE-SIGNING-KEY.pub.asc"
        if cand.exists():
            return cand
    here = Path(__file__).resolve()
    for parent in here.parents:
        cand = parent / "docs" / "RELEASE-SIGNING-KEY.pub.asc"
        if cand.exists():
            return cand
    return None


# ---------------------------------------------------------------------- store

def check_store(repo: str, windows_run: dict[str, Any] | None, hotfix: bool) -> None:
    if hotfix:
        report("INFO", "Microsoft Store", "hotfix: no .msixupload, nothing submitted by design")
        return
    if not windows_run:
        report("SKIP", "Microsoft Store publish step", "no Windows Installer run")
        return
    view = gh_json("run", "view", "--repo", repo, str(windows_run["databaseId"]), "--json", "jobs") or {}
    # The MSIX is built in package-windows, attached in release-windows
    # (#6288) and submitted in publish-store-production, so collect steps
    # across the run's jobs and remember which job owns each.
    jobs = view.get("jobs", [])
    if not any(j["name"] == "release-windows" for j in jobs):
        report("FAIL", "Windows release-windows job found")
        return
    steps, owner = {}, {}
    for j in jobs:
        for s in j.get("steps", []):
            steps.setdefault(s["name"], s)
            owner.setdefault(s["name"], j)
    for name in ("Create MSIX package", "Attach to release"):
        s = steps.get(name)
        c = s["conclusion"] if s else None
        report("PASS" if c == "success" else "FAIL", f"Windows step: {name}", c or "not found")
    s = steps.get(STORE_STEP)
    c = s["conclusion"] if s else None
    if c == "skipped" or s is None:
        report("WARN", f"Windows step: {STORE_STEP}", f"{c or 'not present'} — AETHERSDR_STORE_PRODUCT_ID unset, or the plan marked the version Store-ineligible (check the run's warning annotation)")
        return
    job = owner[STORE_STEP]
    log = run([_tool("gh"), "run", "view", "--repo", repo, "--job", str(job["databaseId"]), "--log"], check=False).stdout
    lines = [re.sub(r"^[^\t]*\t[^\t]*\t", "", ln) for ln in log.splitlines()]
    lines = [re.sub(r"\x1b\[[0-9;]*m", "", ln) for ln in lines]
    if c == "success":
        # AETHERSDR_STORE_AUTO_PUBLISH is a write-only secret, so the
        # publisher's closing line is the only readback of which mode ran.
        if any("Microsoft Store submission committed successfully" in ln for ln in lines):
            detail = "production submission committed to certification (AETHERSDR_STORE_AUTO_PUBLISH armed); nothing to click"
        elif any("Microsoft Store submission staged successfully" in ln for ln in lines):
            detail = "draft staged — AETHERSDR_STORE_AUTO_PUBLISH not armed; Submit to Store is the maintainer's click in Partner Center"
        else:
            detail = "succeeded, but the log has neither the committed nor the staged line; read the job log"
        report("PASS", f"Windows step: {STORE_STEP}", detail)
        return
    start = next((i for i, ln in enumerate(lines) if "Running: msstore publish" in ln and "msixupload" in ln and "aether-store-test" not in ln), None)
    excerpt = ""
    if start is not None:
        tail = [ln.split("Z ", 1)[-1] for ln in lines[start:start + 40]]
        keep = [t for t in tail if t.strip() and not t.startswith(("Retrieving", "This seems", "AppId", "✅"))]
        excerpt = "\n".join(keep[:12])
    recovery = ("recovery: a maintainer deletes or submits the in-progress submission in Partner Center, then uploads the "
                "attached .msixupload by hand; a re-run or a dispatch will NOT submit production")
    if "Ingestion API can only update" in excerpt:
        why = "Partner Center holds a portal-created in-progress submission"
    elif "Azure blob" in excerpt and "0%" in excerpt:
        why = "upload stalled at 0% (the msstore CLI zero-timeout regression; check the CLI pin and --uploadTimeout)"
    else:
        why = "see the quoted log"
    report("FAIL", f"Windows step: {STORE_STEP}", f"{c} — {why}\n{excerpt}\n{recovery}")


# ---------------------------------------------------------------------- main

def main(argv: list[str] | None = None) -> int:
    _utf8_stdio()
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--version", required=True)
    ap.add_argument("--repo", default="aethersdr/AetherSDR")
    ap.add_argument("--download-dir", help="where to download the verification set (default: a temp dir)")
    ap.add_argument("--all", action="store_true", help="download and hash every signable asset, not just the tarball and one AppImage")
    ap.add_argument("--skip-download", action="store_true")
    ap.add_argument("--not-latest", action="store_true", help="this release is expected not to be Latest (a hotfix on an older line)")
    ap.add_argument("--key", help="path to RELEASE-SIGNING-KEY.pub.asc (default: docs/ in the enclosing clone)")
    args = ap.parse_args(argv)

    v = "v" + args.version.lstrip("v")
    parts = v[1:].split(".")
    if not re.fullmatch(r"\d{2}\.\d{1,2}\.\d+(\.\d+)?", v[1:]):
        sys.exit(f"error: {v} is not CalVer YY.M.patch[.hotfix]")
    hotfix = len(parts) == 4 and parts[3] != "0"
    print(f"== {v} ({'hotfix' if hotfix else 'release'}) on {args.repo}\n")

    commit, first_line, tagged_at = check_tag(args.repo, v)
    rel = check_release(args.repo, v, first_line, args.not_latest)
    windows_run, _ = check_runs(args.repo, v, tagged_at)
    if rel:
        tarballs = release_makes_tarballs(args.repo, v)
        app_tarballs = release_makes_app_tarballs(args.repo, v)
        manual_in_sums = release_sums_manual(args.repo, v)
        offloaded = release_offloaded(v)
        assets, on_r2 = check_assets(rel, v, hotfix, tarballs, app_tarballs, manual_in_sums, offloaded)
        check_downloads(args.repo, v, assets, args, tarballs, app_tarballs, manual_in_sums, on_r2)
    check_store(args.repo, windows_run, hotfix)

    fails = [r for r in results if r[0] == "FAIL"]
    warns = [r for r in results if r[0] == "WARN"]
    skips = [r for r in results if r[0] == "SKIP"]
    print(f"\n== {len(results)} checks: {len(fails)} FAIL, {len(warns)} WARN, {len(skips)} SKIP")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
