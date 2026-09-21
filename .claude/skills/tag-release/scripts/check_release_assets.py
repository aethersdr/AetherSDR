#!/usr/bin/env python3
"""check_release_assets.py — the post-tag verification list as one command.

Steps 3, 4 and 5 of the tag-release skill: the tag and its verification, the
release's flags and title, the three build runs and the signing runs, the
asset set with signature timing and the ``.msixupload`` version, the
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
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Any

KEY_FPR = "B7656E6BCB2E022B79F0F97B5578D10E3D5918F3"
SIGNABLE = ("x86_64.AppImage", "aarch64.AppImage", "Windows-x64-setup.exe", "Windows-x64-portable.zip")
BUILD_WORKFLOWS = ("AppImage", "Windows Installer", "macOS DMG")
STORE_STEP = "Stage Microsoft Store submission (draft)"

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
    extra = sorted({r["workflowName"] for r in runs} - set(BUILD_WORKFLOWS))
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
        wanted = {k: steps.get(k) for k in ("Code sign application", "Sign DMG", "Notarize DMG", "Attach to release")}
        if all(c == "success" for c in wanted.values()):
            report("PASS", f"macOS notarization steps: {job['name']}", ", ".join(f"{k} {c}" for k, c in wanted.items()))
        else:
            report("FAIL", f"macOS notarization steps: {job['name']}", ", ".join(f"{k} {c}" for k, c in wanted.items()))


# --------------------------------------------------------------------- assets

def expected_assets(v: str, hotfix: bool) -> list[str]:
    bare = ".".join(v.lstrip("v").split(".")[:3])  # 26.9.5 and 26.9.5.0 both package as 26.9.5.0
    names = [f"AetherSDR-{v}-x86_64.AppImage", f"AetherSDR-{v}-aarch64.AppImage",
             f"AetherSDR-{v}-macOS-apple-silicon.dmg", f"AetherSDR-{v}-macOS-intel.dmg",
             f"AetherSDR-{v}-Windows-x64-setup.exe", f"AetherSDR-{v}-Windows-x64-portable.zip",
             f"AetherSDR-{v}-source.tar.gz", "SHA256SUMS.txt"]
    signed = [n + ".asc" for n in names if not n.endswith(".dmg")]
    out = names + signed
    if not hotfix:
        out.append(f"AetherSDR-{bare}.0-Windows-x64.msixupload")
    return out


def check_assets(rel: dict[str, Any], v: str, hotfix: bool) -> dict[str, dict[str, Any]]:
    assets = {a["name"]: a for a in rel.get("assets", [])}
    want = expected_assets(v, hotfix)
    missing = [n for n in want if n not in assets]
    n_want = len(want)
    if missing:
        report("FAIL", f"the {n_want}-asset set is present", "missing: " + ", ".join(missing))
    else:
        report("PASS", f"the {n_want}-asset set is present ({'hotfix: no .msixupload' if hotfix else 'incl. .msixupload'})")
    extra = sorted(set(assets) - set(want))
    if extra:
        bad = [e for e in extra if "msixupload" in e]
        report("FAIL" if bad else "WARN", "no unexpected assets", ", ".join(extra))
    # The four CI-built binaries are attached by other workflows before the
    # signing job runs, so their .asc must be strictly newer. The tarball and
    # SHA256SUMS.txt are produced by the signing job and uploaded in the same
    # `gh release upload` as their signatures, so their timestamps tie (either
    # order, within a few seconds); only a real gap there means a re-sign
    # clobbered one file and not the other.
    built = [f"AetherSDR-{v}-{s}" for s in SIGNABLE]
    late, gaps = [], []
    for n in want:
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
    uploaders = sorted({a["uploader"]["login"] for a in assets.values()})
    if uploaders != ["github-actions[bot]"]:
        report("WARN", "every asset was uploaded by CI", "uploaders: " + ", ".join(uploaders))
    return assets


# ------------------------------------------------------------------- downloads

def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def check_downloads(repo: str, v: str, assets: dict[str, Any], args: argparse.Namespace) -> None:
    if args.skip_download:
        report("SKIP", "download and verify signatures and checksums", "--skip-download")
        return
    ddir = Path(args.download_dir or tempfile.mkdtemp(prefix=f"aethersdr-{v}-"))
    ddir.mkdir(parents=True, exist_ok=True)
    patterns = ["SHA256SUMS.txt", "SHA256SUMS.txt.asc", f"AetherSDR-{v}-source.tar.gz", f"AetherSDR-{v}-source.tar.gz.asc",
                f"AetherSDR-{v}-x86_64.AppImage", f"AetherSDR-{v}-x86_64.AppImage.asc"]
    if args.all:
        patterns = [n for n in assets if not n.endswith(".dmg") and not n.endswith(".msixupload")]
    patterns = [p for p in patterns if p in assets]
    argv = ["release", "download", v, "--repo", repo, "--dir", str(ddir), "--clobber"]
    for p in patterns:
        argv += ["--pattern", p]
    proc = run([_tool("gh"), *argv], check=False)
    got = [p for p in patterns if (ddir / p).exists()]
    if proc.returncode != 0 or len(got) != len(patterns):
        report("FAIL", "download the verification set", (proc.stderr or "").strip() or f"got {len(got)}/{len(patterns)}")
        return
    report("INFO", "downloaded", f"{len(got)} files into {ddir}")

    sums_path = ddir / "SHA256SUMS.txt"
    if sums_path.exists():
        entries = {}
        for line in sums_path.read_text(encoding="utf-8").splitlines():
            m = re.match(r"^([0-9a-f]{64})\s+\*?(.+)$", line.strip())
            if m:
                entries[m.group(2)] = m.group(1)
        want = [f"AetherSDR-{v}-{s}" for s in SIGNABLE] + [f"AetherSDR-{v}-source.tar.gz"]
        if sorted(entries) == sorted(want):
            report("PASS", "SHA256SUMS.txt covers exactly the five signable files")
        else:
            report("FAIL", "SHA256SUMS.txt covers exactly the five signable files",
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
        for name in ["SHA256SUMS.txt", f"AetherSDR-{v}-source.tar.gz", f"AetherSDR-{v}-x86_64.AppImage"]:
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
        report("INFO", "Microsoft Store", "hotfix: no .msixupload, nothing staged by design")
        return
    if not windows_run:
        report("SKIP", "Microsoft Store staging step", "no Windows Installer run")
        return
    view = gh_json("run", "view", "--repo", repo, str(windows_run["databaseId"]), "--json", "jobs") or {}
    job = next((j for j in view.get("jobs", []) if j["name"] == "build-windows"), None)
    if not job:
        report("FAIL", "Windows build-windows job found")
        return
    steps = {s["name"]: s for s in job.get("steps", [])}
    for name in ("Create MSIX package", "Attach to release"):
        s = steps.get(name)
        c = s["conclusion"] if s else None
        report("PASS" if c == "success" else "FAIL", f"Windows step: {name}", c or "not found")
    s = steps.get(STORE_STEP)
    c = s["conclusion"] if s else None
    if c == "success":
        report("PASS", f"Windows step: {STORE_STEP}", "draft staged — Submit to Store is the maintainer's click in Partner Center")
        return
    if c == "skipped" or s is None:
        report("WARN", f"Windows step: {STORE_STEP}", f"{c or 'not present'} — AETHERSDR_STORE_PRODUCT_ID unset, or the plan marked the version Store-ineligible (check the run's warning annotation)")
        return
    log = run([_tool("gh"), "run", "view", "--repo", repo, "--job", str(job["databaseId"]), "--log"], check=False).stdout
    lines = [re.sub(r"^[^\t]*\t[^\t]*\t", "", ln) for ln in log.splitlines()]
    lines = [re.sub(r"\x1b\[[0-9;]*m", "", ln) for ln in lines]
    start = next((i for i, ln in enumerate(lines) if "Running: msstore publish" in ln and "msixupload" in ln and "aether-store-test" not in ln), None)
    excerpt = ""
    if start is not None:
        tail = [ln.split("Z ", 1)[-1] for ln in lines[start:start + 40]]
        keep = [t for t in tail if t.strip() and not t.startswith(("Retrieving", "This seems", "AppId", "✅"))]
        excerpt = "\n".join(keep[:12])
    recovery = ("recovery: a maintainer deletes or submits the in-progress submission in Partner Center, then uploads the "
                "attached .msixupload by hand; a re-run or a dispatch will NOT stage production")
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
        assets = check_assets(rel, v, hotfix)
        check_downloads(args.repo, v, assets, args)
    check_store(args.repo, windows_run, hotfix)

    fails = [r for r in results if r[0] == "FAIL"]
    warns = [r for r in results if r[0] == "WARN"]
    skips = [r for r in results if r[0] == "SKIP"]
    print(f"\n== {len(results)} checks: {len(fails)} FAIL, {len(warns)} WARN, {len(skips)} SKIP")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
