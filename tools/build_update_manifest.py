#!/usr/bin/env python3
"""
Build the in-app update manifest and its patch bundles for one release
(RFC #5304 Phase 2).

The update unit is the payload tree, not the shipped container: the files
inside the Windows portable zip (the same deploy\\ tree setup.exe installs) and
inside each AppImage's squashfs (the tree the Linux tarball channel will ship).
Between two releases only a handful of those files change, so each release
carries, per payload and per earlier release, one bundle of per-file
`zstd --patch-from` deltas.

Outputs, in --out:

  manifest.json                 schema 1, described in
                                docs/architecture/update-manifest.md
  <payload>-from-<tag>.update.tar   one bundle per (payload, earlier release)

The manifest is NOT signed here. The publish job signs it with the Ed25519
update key and verifies the signature against packaging/update-signing.pub
before anything is uploaded.

Usage (CI):
    python3 tools/build_update_manifest.py --tag v26.10.4 --out out \\
        --fetch-from-github --depth 4 --public-key packaging/update-signing.pub

Usage (local, assets already downloaded per release):
    python3 tools/build_update_manifest.py --tag v26.10.4 --out out \\
        --assets v26.10.4=dl/v26.10.4 --assets v26.10.3=dl/v26.10.3

Needs python3 >= 3.10, zstd >= 1.5 and (for AppImages) unsquashfs. Nothing
here executes a downloaded file: an AppImage's squashfs is located from its
ELF header and read with unsquashfs, so an aarch64 image is handled on an
x86_64 runner.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import math
import os
import re
import shutil
import stat
import struct
import subprocess
import sys
import tarfile
import tempfile
import zipfile
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path, PurePosixPath

SCHEMA = 1
# The repository whose releases are read and named (a fork's dry run reads
# its own); the app's allowlist accepts only aethersdr/AetherSDR's.
REPO = os.environ.get("GITHUB_REPOSITORY", "aethersdr/AetherSDR")
DOWNLOAD_BASE = f"https://github.com/{REPO}/releases/download/"
# A stable release: vYY.M.patch, with an optional hotfix (v26.7.4.1).
TAG_RE = re.compile(r"^v(\d+)\.(\d+)\.(\d+)(?:\.(\d+))?$")

# zstd's --patch-from needs a window that covers the reference file. Recorded
# per patch so the applier can raise ZSTD_d_windowLogMax to exactly that.
MIN_WINDOW_LOG = 10
MAX_WINDOW_LOG = 31
ZSTD_LEVEL = 19


@dataclass(frozen=True)
class PayloadKind:
    """One updatable payload: which asset carries it and how to open it."""

    platform: str   # "windows", "linux"
    arch: str       # as QSysInfo::buildCpuArchitecture() names it
    asset_suffix: str
    container: str  # "zip" or "appimage"
    label: str      # used in bundle names

    def asset_name(self, tag: str) -> str:
        return f"AetherSDR-{tag}-{self.asset_suffix}"


PAYLOADS = (
    PayloadKind("windows", "x86_64", "Windows-x64-portable.zip", "zip", "Windows-x64"),
    PayloadKind("linux", "x86_64", "x86_64.AppImage", "appimage", "x86_64"),
    PayloadKind("linux", "arm64", "aarch64.AppImage", "appimage", "aarch64"),
)


def version_key(tag: str) -> tuple[int, int, int, int]:
    """Comparable parts of a release tag; an absent hotfix counts as 0."""
    m = TAG_RE.match(tag)
    if not m:
        raise ValueError(f"not a release tag: {tag!r}")
    return tuple(int(g or 0) for g in m.groups())  # type: ignore[return-value]


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def window_log_for(size: int) -> int:
    """The smallest zstd window log that covers `size` bytes, clamped."""
    if size <= 1:
        return MIN_WINDOW_LOG
    return max(MIN_WINDOW_LOG, min(MAX_WINDOW_LOG, math.ceil(math.log2(size))))


# ── Payload trees ────────────────────────────────────────────────────────────

@dataclass(frozen=True)
class Entry:
    type: str            # "file" or "symlink"
    size: int = 0
    sha256: str = ""
    executable: bool = False
    target: str = ""     # symlink target, as stored


def scan_tree(root: Path) -> dict[str, Entry]:
    """Every file and symlink under root, keyed by forward-slash path.

    Directories are implied by their contents; anything that is neither a
    regular file nor a symlink (a device, a FIFO) is refused rather than
    silently dropped."""
    out: dict[str, Entry] = {}
    for dirpath, dirnames, filenames in os.walk(root, followlinks=False):
        dirnames.sort()
        here = Path(dirpath)
        names = sorted(filenames) + sorted(d for d in dirnames if (here / d).is_symlink())
        for name in names:
            p = here / name
            rel = p.relative_to(root).as_posix()
            st = p.lstat()
            if stat.S_ISLNK(st.st_mode):
                target = os.readlink(p)
                if not symlink_stays_inside(rel, target):
                    raise ValueError(f"{rel}: symlink target {target!r} leaves the payload")
                out[rel] = Entry("symlink", target=target)
            elif stat.S_ISREG(st.st_mode):
                out[rel] = Entry("file", st.st_size, sha256_file(p),
                                 bool(st.st_mode & 0o111))
            else:
                raise ValueError(f"{rel}: not a regular file or symlink")
    return out


def symlink_stays_inside(rel: str, target: str) -> bool:
    """A link the manifest may carry: relative, and resolving (lexically,
    from the link's own directory) to a path inside the payload."""
    if not target or PurePosixPath(target).is_absolute() or "\\" in target:
        return False
    depth = len(PurePosixPath(rel).parts) - 1
    for part in PurePosixPath(target).parts:
        depth += -1 if part == ".." else (0 if part == "." else 1)
        if depth < 0:
            return False
    return True


def appimage_squashfs_offset(path: Path) -> int:
    """Where the squashfs starts in a type-2 AppImage: the end of its ELF
    runtime, computed from the section header table as the runtime itself
    does, so the image is never executed."""
    with path.open("rb") as f:
        ident = f.read(16)
        if ident[:4] != b"\x7fELF":
            raise ValueError(f"{path.name}: not an ELF file")
        is64 = ident[4] == 2
        endian = "<" if ident[5] == 1 else ">"
        if is64:
            f.seek(0x28)
            shoff = struct.unpack(endian + "Q", f.read(8))[0]
            f.seek(0x3A)
        else:
            f.seek(0x20)
            shoff = struct.unpack(endian + "I", f.read(4))[0]
            f.seek(0x2E)
        shentsize, shnum = struct.unpack(endian + "HH", f.read(4))
        offset = shoff + shentsize * shnum
        f.seek(offset)
        if f.read(4) != b"hsqs":
            raise ValueError(f"{path.name}: no squashfs at offset {offset}")
        return offset


def extract_payload(asset: Path, container: str, dest: Path) -> Path:
    """Unpack a payload into dest and return the tree's root."""
    dest.mkdir(parents=True, exist_ok=True)
    if container == "zip":
        with zipfile.ZipFile(asset) as z:
            for info in z.infolist():
                name = PurePosixPath(info.filename.replace("\\", "/"))
                if name.is_absolute() or ".." in name.parts:
                    raise ValueError(f"{asset.name}: unsafe path {info.filename!r}")
            z.extractall(dest)
        return dest
    if container == "appimage":
        root = dest / "squashfs-root"
        subprocess.run(["unsquashfs", "-q", "-n", "-no-xattrs", "-o",
                        str(appimage_squashfs_offset(asset)), "-d", str(root), str(asset)],
                       check=True, stdout=subprocess.DEVNULL)
        return root
    raise ValueError(f"unknown container {container!r}")


# ── Patches ─────────────────────────────────────────────────────────────────

def zstd_patch(old: Path, new: Path, out: Path, window_log: int) -> None:
    subprocess.run(["zstd", "-q", "-f", f"-{ZSTD_LEVEL}", f"--long={window_log}",
                    f"--patch-from={old}", str(new), "-o", str(out)], check=True)


def zstd_whole(new: Path, out: Path, window_log: int) -> None:
    subprocess.run(["zstd", "-q", "-f", f"-{ZSTD_LEVEL}", f"--long={window_log}",
                    str(new), "-o", str(out)], check=True)


def deterministic_tar(members: list[tuple[str, Path]], out: Path) -> None:
    """A tar whose bytes depend only on its members' names and contents."""
    with tarfile.open(out, "w", format=tarfile.USTAR_FORMAT) as tar:
        for name, path in members:
            info = tarfile.TarInfo(name)
            info.size = path.stat().st_size
            info.mode = 0o644
            info.mtime = 0
            info.uid = info.gid = 0
            info.uname = info.gname = ""
            with path.open("rb") as f:
                tar.addfile(info, f)


def build_patch_set(old_root: Path, old: dict[str, Entry], new_root: Path,
                    new: dict[str, Entry], bundle: Path, work: Path) -> dict | None:
    """The ops that turn the `old` tree into the `new` one, packed into
    `bundle`. None when the trees are identical (no bundle is written).

    Removals come first, so a path that changes between file, directory and
    symlink is cleared before its replacement is written (the applier prunes
    directories the removals leave empty)."""
    ops: list[dict] = [{"op": "remove", "path": path} for path in sorted(set(old) - set(new))]
    members: list[tuple[str, Path]] = []
    work.mkdir(parents=True, exist_ok=True)
    unchanged = 0

    for path in sorted(new):
        n = new[path]
        o = old.get(path)
        if o == n:
            unchanged += 1
            continue
        if n.type == "symlink":
            ops.append({"op": "symlink", "path": path, "target": n.target})
            continue
        member = f"{len(members):06d}.zst"
        tmp = work / member
        window = window_log_for(n.size)
        if o is not None and o.type == "file":
            window = window_log_for(max(o.size, n.size))
            zstd_patch(old_root / path, new_root / path, tmp, window)
            whole = work / (member + ".whole")
            zstd_whole(new_root / path, whole, window_log_for(n.size))
            if whole.stat().st_size < tmp.stat().st_size:
                whole.replace(tmp)
                window = window_log_for(n.size)
                op = {"op": "add", "path": path}
            else:
                whole.unlink()
                op = {"op": "patch", "path": path, "from_sha256": o.sha256}
        else:
            zstd_whole(new_root / path, tmp, window)
            op = {"op": "add", "path": path}
        op.update({"member": member, "window_log": window, "sha256": n.sha256,
                   "size": n.size, "executable": n.executable})
        ops.append(op)
        members.append((member, tmp))

    if not ops:
        return None
    deterministic_tar(members, bundle)
    for _, p in members:
        p.unlink()
    return {"ops": ops, "unchanged": unchanged}


def verify_patch_set(old_root: Path, bundle: Path, patch: dict, files: list[dict],
                     work: Path) -> None:
    """Apply `bundle` to a copy of the old tree, by the documented contract,
    and check every file of the new one. Run on the real release pair before
    anything is published; raises on any mismatch."""
    dest = work / "applied"
    members = work / "members"
    shutil.rmtree(work, ignore_errors=True)
    shutil.copytree(old_root, dest, symlinks=True)
    members.mkdir(parents=True)
    with tarfile.open(bundle) as tar:
        tar.extractall(members, filter="data")
    pruned = False
    for op in patch["ops"]:
        target = dest / op["path"]
        if op["op"] == "remove":
            target.unlink()
            continue
        if not pruned:
            # After the removals, drop every directory left empty. Bottom-up,
            # so a parent emptied by pruning its children goes too
            # (plugins/sub/x.dll removed before plugins becomes a file).
            for dp, _, _ in os.walk(dest, topdown=False):
                d = Path(dp)
                if d != dest and not any(d.iterdir()):
                    d.rmdir()
            pruned = True
        if op["op"] == "symlink":
            if target.is_symlink() or target.exists():
                target.unlink()
            target.parent.mkdir(parents=True, exist_ok=True)
            os.symlink(op["target"], target)
            continue
        out = members / (op["member"] + ".out")
        cmd = ["zstd", "-q", "-d", "-f", f"--long={op['window_log']}"]
        if op["op"] == "patch":
            if sha256_file(target) != op["from_sha256"]:
                raise ValueError(f"{op['path']}: source does not match from_sha256")
            cmd.append(f"--patch-from={target}")
        subprocess.run(cmd + [str(members / op["member"]), "-o", str(out)], check=True)
        if sha256_file(out) != op["sha256"] or out.stat().st_size != op["size"]:
            raise ValueError(f"{op['path']}: result does not match its sha256/size")
        if target.is_symlink():
            target.unlink()
        target.parent.mkdir(parents=True, exist_ok=True)
        out.replace(target)
    applied = scan_tree(dest)
    want = {f["path"]: f for f in files}
    if set(applied) != set(want):
        raise ValueError(f"applied tree differs in paths: {sorted(set(applied) ^ set(want))[:5]}")
    for path, e in applied.items():
        f = want[path]
        same = (e.type == f["type"] and (e.target == f["target"] if e.type == "symlink"
                                         else e.sha256 == f["sha256"]))
        if not same:
            raise ValueError(f"{path}: applied file does not match the manifest")
    shutil.rmtree(work, ignore_errors=True)


# ── Releases ────────────────────────────────────────────────────────────────

def gh(*args: str) -> str:
    return subprocess.run(["gh", *args], check=True, capture_output=True, text=True).stdout


def earlier_releases(tag: str, depth: int) -> list[str]:
    """The `depth` newest published releases before `tag`: release-shaped
    tags only, no drafts or pre-releases (test builds share the release
    list), newest first."""
    lines = gh("api", "--paginate", f"repos/{REPO}/releases?per_page=100",
               "--jq", ".[] | {tag: .tag_name, draft, prerelease}").splitlines()
    data = [json.loads(l) for l in lines if l.strip()]
    target = version_key(tag)
    tags = [r["tag"] for r in data
            if not r["draft"] and not r["prerelease"] and TAG_RE.match(r["tag"])
            and version_key(r["tag"]) < target]
    return sorted(set(tags), key=version_key, reverse=True)[:depth]


def fetch_asset(tag: str, name: str, dest: Path, attempts: int = 3) -> Path | None:
    """Download one release asset, retrying a failed transfer. None when it
    still fails; the caller decides whether that is fatal."""
    dest.mkdir(parents=True, exist_ok=True)
    path = dest / name
    for attempt in range(1, attempts + 1):
        result = subprocess.run(["gh", "release", "download", tag, "--repo", REPO,
                                 "--pattern", name, "--dir", str(dest), "--clobber"],
                                capture_output=True, text=True)
        if result.returncode == 0 and path.exists():
            return path
        print(f"{tag}/{name}: download attempt {attempt} failed: {result.stderr.strip()}",
              file=sys.stderr)
    return None


def local_asset(dirs: dict[str, Path], tag: str, name: str) -> Path | None:
    d = dirs.get(tag)
    if d is None:
        return None
    p = d / name
    return p if p.exists() else None


# ── Manifest ────────────────────────────────────────────────────────────────

def key_id(public_key: Path | None) -> str:
    """The first 16 hex digits of the SHA-256 of the raw 32-byte Ed25519
    public key, so a rotated key is told apart from the one it replaced."""
    if public_key is None:
        return ""
    lines = [l for l in public_key.read_text().splitlines() if l and not l.startswith("-----")]
    der = base64.b64decode("".join(lines))
    return hashlib.sha256(der[-32:]).hexdigest()[:16]


def files_list(tree: dict[str, Entry]) -> list[dict]:
    out = []
    for path in sorted(tree):
        e = tree[path]
        if e.type == "symlink":
            out.append({"path": path, "type": "symlink", "target": e.target})
        else:
            out.append({"path": path, "type": "file", "size": e.size, "sha256": e.sha256,
                        "executable": e.executable})
    return out


class MissingPayload(RuntimeError):
    """A payload the release must carry could not be had."""


def build(tag: str, out: Path, depth: int, get_asset, earlier: list[str],
          public_key: Path | None, work_root: Path, discard_assets: bool = False,
          require_all: bool = False) -> dict:
    """discard_assets: delete each downloaded asset once used (CI disk);
    never set for assets the caller supplied.

    require_all: every payload in PAYLOADS must be present in `tag`, or the
    build fails (MissingPayload) instead of writing a manifest that silently
    leaves a platform out. A missing EARLIER release only loses that patch."""
    out.mkdir(parents=True, exist_ok=True)
    manifest = {
        "schema": SCHEMA,
        "product": "AetherSDR",
        "tag": tag,
        "version": tag[1:],
        "generated_at": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "key_id": key_id(public_key),
        "payloads": [],
    }
    for kind in PAYLOADS:
        asset = get_asset(tag, kind.asset_name(tag))
        if asset is None:
            if require_all:
                raise MissingPayload(f"{kind.asset_name(tag)}: could not be fetched from {tag}")
            print(f"{kind.asset_name(tag)}: missing from {tag}; payload left out", file=sys.stderr)
            continue
        work = work_root / kind.label
        shutil.rmtree(work, ignore_errors=True)
        new_root = extract_payload(asset, kind.container, work / "new")
        new_tree = scan_tree(new_root)
        entry = {
            "platform": kind.platform,
            "arch": kind.arch,
            "full": {"asset": asset.name, "url": DOWNLOAD_BASE + f"{tag}/{asset.name}",
                     "sha256": sha256_file(asset), "size": asset.stat().st_size},
            "files": files_list(new_tree),
            "patches": [],
        }
        for old_tag in earlier[:depth]:
            old_asset = get_asset(old_tag, kind.asset_name(old_tag))
            if old_asset is None:
                print(f"{kind.asset_name(old_tag)}: missing; no patch from {old_tag}", file=sys.stderr)
                continue
            old_root = extract_payload(old_asset, kind.container, work / "old")
            bundle_name = f"AetherSDR-{tag}-{kind.label}-from-{old_tag}.update.tar"
            ops = build_patch_set(old_root, scan_tree(old_root), new_root, new_tree,
                                  out / bundle_name, work / "patches")
            if ops is not None:
                # The real pair, by the documented contract, before anything
                # is signed: a bundle that does not rebuild the release fails
                # the build here.
                verify_patch_set(old_root, out / bundle_name, ops, entry["files"], work / "verify")
            shutil.rmtree(work / "old", ignore_errors=True)
            if discard_assets:
                old_asset.unlink(missing_ok=True)
            if ops is None:
                continue
            bundle = out / bundle_name
            entry["patches"].append({
                "from": old_tag[1:],
                "bundle": {"asset": bundle_name, "url": DOWNLOAD_BASE + f"{tag}/{bundle_name}",
                           "sha256": sha256_file(bundle), "size": bundle.stat().st_size},
                **ops,
            })
            print(f"{bundle_name}: {bundle.stat().st_size} bytes, "
                  f"{len(ops['ops'])} ops, {ops['unchanged']} unchanged", file=sys.stderr)
        shutil.rmtree(work, ignore_errors=True)
        if discard_assets:
            asset.unlink(missing_ok=True)
        manifest["payloads"].append(entry)

    (out / "manifest.json").write_text(json.dumps(manifest, indent=1, sort_keys=False) + "\n")
    return manifest


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--tag", required=True)
    ap.add_argument("--out", required=True, type=Path)
    ap.add_argument("--depth", type=int, default=4)
    ap.add_argument("--public-key", type=Path)
    ap.add_argument("--fetch-from-github", action="store_true")
    ap.add_argument("--assets", action="append", default=[], metavar="TAG=DIR",
                    help="a directory already holding a release's assets")
    ap.add_argument("--work", type=Path, help="scratch directory (default: a temp dir)")
    ap.add_argument("--require-all", action="store_true",
                    help="fail unless every payload is present (implied by --fetch-from-github)")
    args = ap.parse_args(argv)

    version_key(args.tag)
    if args.public_key is not None and not args.public_key.exists():
        args.public_key = None

    if args.fetch_from_github:
        earlier = earlier_releases(args.tag, args.depth)
        dl_root = Path(tempfile.mkdtemp(prefix="update-dl-"))
        get_asset = lambda t, n: fetch_asset(t, n, dl_root / t)  # noqa: E731
    else:
        dirs = {}
        for spec in args.assets:
            t, _, d = spec.partition("=")
            version_key(t)
            dirs[t] = Path(d)
        earlier = sorted((t for t in dirs if version_key(t) < version_key(args.tag)),
                         key=version_key, reverse=True)
        get_asset = lambda t, n: local_asset(dirs, t, n)  # noqa: E731

    work = args.work or Path(tempfile.mkdtemp(prefix="update-work-"))
    try:
        manifest = build(args.tag, args.out, args.depth, get_asset, earlier, args.public_key, work,
                         discard_assets=args.fetch_from_github,
                         require_all=args.require_all or args.fetch_from_github)
    except MissingPayload as e:
        print(f"error: {e}; no manifest written", file=sys.stderr)
        return 1
    if not manifest["payloads"]:
        print("no payloads found", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
