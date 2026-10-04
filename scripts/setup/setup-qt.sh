#!/bin/bash
# setup-qt.sh — Install the exact Qt AetherSDR releases are built with, for a
# source build on Linux or macOS.
#
# AetherSDR requires Qt 6.12 (QT_SOURCE_FLOOR in cmake/qt-pin.env), the Qt
# every release is built against, and few distros package it yet. This is the
# supported way to get it for a source build; a distro or Qt-installer Qt that
# is already 6.12+ works too.
#
# It installs the pinned version from cmake/qt-pin.env — the same file that
# every CI leg and release workflow is checked against — with the same pinned
# aqtinstall, into a per-user cache shared by every checkout:
#
#   Linux:  ${XDG_CACHE_HOME:-~/.cache}/aethersdr/qt/
#   macOS:  ~/Library/Caches/aethersdr/qt/
#
# (override with AETHER_QT_CACHE). Each install is an immutable generation,
# gen/<version>-<revision>-<id>/, and <version>-<revision>.current names the
# live one. A reinstall builds a new generation beside the old and publishes it
# by atomically replacing that pointer file, so every checkout sees either the
# old kit or the new one and never neither (Constitution XIV). Superseded
# generations are kept: binaries already built in any checkout link their Qt by
# absolute path and must keep launching until they are rebuilt, so nothing is
# deleted until you ask with --prune. No tree is ever renamed,
# so the paths aqt patches into the kit (qmake, CMake, .pc files) stay true.
# CMake reads the pointer on its own
# (cmake/AetherQtPin.cmake), so after this script a plain `cmake -B build`
# picks the pinned Qt up with no flags.
#
# Then it builds qtkeychain against that Qt (setup-qtkeychain.sh). A
# qtkeychain built against a different Qt is an ABI mismatch, and leaving it
# out silently compiles SmartLink credential persistence away (#3639).
#
# The checks run BEFORE the 2 GB download, so an unsupported machine is told
# what is wrong and what to do instead, rather than discovering it as a loader
# error after the fact:
#   - glibc new enough for Qt's Linux binaries (QT_MIN_GLIBC_* in the pin file)
#   - Xcode new enough for Qt's macOS SDK check (QT_MIN_XCODE)
#   - a working `python3 -m venv` (Debian/Ubuntu split it into python3-venv)
#   - free disk space
#   - the repository still serving the pinned package revision
#
# Usage:
#   scripts/setup/setup-qt.sh                 install (no-op if already present)
#   scripts/setup/setup-qt.sh --print-prefix  print the Qt prefix CMake will use
#   scripts/setup/setup-qt.sh --no-keychain   skip the qtkeychain build
#   scripts/setup/setup-qt.sh --prune         delete every generation but the
#                                             live one (frees ~2 GB each; builds
#                                             against a pruned one must rebuild)
#
# Requires: python3, curl, and for qtkeychain: git, cmake, ninja, a C++
# compiler. Windows builders: see docs/BUILDING.md (Qt online installer or aqt).

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PIN_FILE="$REPO_ROOT/cmake/qt-pin.env"

die() { echo "ERROR: $*" >&2; exit 1; }

[ -f "$PIN_FILE" ] || die "$PIN_FILE is missing."
# shellcheck source=../../cmake/qt-pin.env
. "$PIN_FILE"

PRINT_PREFIX=0
WITH_KEYCHAIN=1
PRUNE=0
for arg in "$@"; do
    case "$arg" in
        --print-prefix) PRINT_PREFIX=1 ;;
        --no-keychain)  WITH_KEYCHAIN=0 ;;
        --prune)        PRUNE=1 ;;
        -h|--help)      sed -n '2,/^set -euo/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
        *)              die "unknown argument: $arg (try --help)" ;;
    esac
done

# ── Platform → aqt host/arch and the directory aqt lays the kit down in ──
OS="$(uname -s)"
MACHINE="$(uname -m)"
case "$OS/$MACHINE" in
    Linux/x86_64)
        AQT_HOST=linux; AQT_ARCH=linux_gcc_64; KIT_DIR=gcc_64
        REPO_HOST=linux_x64; MIN_GLIBC="$QT_MIN_GLIBC_X86_64" ;;
    Linux/aarch64|Linux/arm64)
        AQT_HOST=linux_arm64; AQT_ARCH=linux_gcc_arm64; KIT_DIR=gcc_arm64
        REPO_HOST=linux_arm64; MIN_GLIBC="$QT_MIN_GLIBC_AARCH64" ;;
    Darwin/*)
        # clang_64 is universal2 — one kit for Intel and Apple Silicon.
        AQT_HOST=mac; AQT_ARCH=clang_64; KIT_DIR=macos
        REPO_HOST=mac_x64; MIN_GLIBC="" ;;
    *)
        die "Qt publishes no desktop binaries for $OS/$MACHINE. Build Qt $QT_SOURCE_FLOOR+
       from source, or use a distro Qt $QT_SOURCE_FLOOR+ if yours ships one." ;;
esac

if [ "$OS" = "Darwin" ]; then
    CACHE_ROOT="${AETHER_QT_CACHE:-$HOME/Library/Caches/aethersdr/qt}"
else
    CACHE_ROOT="${AETHER_QT_CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/aethersdr/qt}"
fi
# The revision is part of every name: RC and final share a version string, and
# a rebuilt Qt behind an unchanged pin must never be mistaken for the old one.
PIN_TAG="$QT_VERSION-$QT_PACKAGE_REVISION"
POINTER="$CACHE_ROOT/$PIN_TAG.current"
GEN_ROOT="$CACHE_ROOT/gen"
STAMP_CONTENT="version=$QT_VERSION revision=$QT_PACKAGE_REVISION arch=$AQT_ARCH modules=$QT_MODULES aqt=$AQTINSTALL_VERSION"

# The live generation's directory name, or nothing.
CURRENT_GEN=""
if [ -f "$POINTER" ]; then
    CURRENT_GEN="$(head -n 1 "$POINTER")"
fi
prefix_of() { echo "$GEN_ROOT/$1/$QT_VERSION/$KIT_DIR"; }
QT_PREFIX=""
[ -n "$CURRENT_GEN" ] && QT_PREFIX="$(prefix_of "$CURRENT_GEN")"

# ── --prune: reclaim superseded generations, on request only ────────────
# Keeps the live generation of the current pin and any unfinished generation
# whose owning install is still running (another checkout, mid-install).
# Everything else under gen/ goes, including generations of earlier pins, along
# with pointer files that would be left naming them.
if [ "$PRUNE" = 1 ]; then
    removed=0
    for d in "$GEN_ROOT"/*; do
        [ -d "$d" ] || continue
        name="${d##*/}"
        [ "$name" = "$CURRENT_GEN" ] && continue
        if [ ! -f "$d/.aether-qt-stamp" ] && kill -0 "${name##*-}" 2>/dev/null; then
            echo "Keeping $name: an install is still running in it"
            continue
        fi
        echo "Removing $name"
        rm -rf "${d:?}"
        removed=$((removed + 1))
    done
    for ptr in "$CACHE_ROOT"/*.current; do
        [ -f "$ptr" ] || continue
        [ "$ptr" = "$POINTER" ] && continue
        [ -d "$GEN_ROOT/$(head -n 1 "$ptr")" ] || rm -f "$ptr"
    done
    echo "Pruned $removed generation(s). Live: ${CURRENT_GEN:-none}"
    echo "Builds configured against a pruned generation re-run CMake on their"
    echo "next build and relink against the live one."
    exit 0
fi

if [ "$PRINT_PREFIX" = 1 ]; then
    if [ -n "$QT_PREFIX" ] && [ -x "$QT_PREFIX/bin/qmake" ]; then
        echo "$QT_PREFIX"
        exit 0
    fi
    echo "Qt $QT_VERSION is not installed; run scripts/setup/setup-qt.sh" >&2
    exit 1
fi

# True when version $1 is older than $2.
older_than() {
    [ "$1" != "$2" ] && [ "$(printf '%s\n%s\n' "$1" "$2" | sort -V | head -1)" = "$1" ]
}

build_keychain() {
    [ "$WITH_KEYCHAIN" = 1 ] || return 0
    echo
    echo "Building qtkeychain against Qt $QT_VERSION (skip with --no-keychain)..."
    (cd "$REPO_ROOT" && CMAKE_PREFIX_PATH="$QT_PREFIX" bash scripts/setup/setup-qtkeychain.sh)
}

# ── Already installed? ───────────────────────────────────────────────────
if [ -n "$CURRENT_GEN" ] && [ -f "$GEN_ROOT/$CURRENT_GEN/.aether-qt-stamp" ] &&
   [ "$(cat "$GEN_ROOT/$CURRENT_GEN/.aether-qt-stamp")" = "$STAMP_CONTENT" ] &&
   [ -x "$QT_PREFIX/bin/qmake" ] &&
   [ "$("$QT_PREFIX/bin/qmake" -query QT_VERSION)" = "$QT_VERSION" ]; then
    echo "Qt $QT_VERSION ($QT_PACKAGE_REVISION) already installed at $QT_PREFIX"
    build_keychain
    exit 0
fi

# ── Preflight: refuse before downloading, with the way out ───────────────
if [ -n "$MIN_GLIBC" ]; then
    GLIBC="$(getconf GNU_LIBC_VERSION 2>/dev/null | awk '{print $2}')"
    if [ -z "$GLIBC" ]; then
        die "this system's C library is not glibc (musl?). Qt's Linux binaries
       need glibc $MIN_GLIBC+. Use a distro Qt $QT_SOURCE_FLOOR+ if yours ships one,
       or build Qt from source."
    fi
    if older_than "$GLIBC" "$MIN_GLIBC"; then
        die "Qt $QT_VERSION's $MACHINE binaries need glibc $MIN_GLIBC; this system has $GLIBC.
       They would install, then fail to load. Options: a distro Qt $QT_SOURCE_FLOOR+,
       a newer OS release, or the AetherSDR AppImage (no build needed)."
    fi
    echo "glibc $GLIBC — OK (Qt $QT_VERSION needs $MIN_GLIBC)"
fi

if [ "$OS" = "Darwin" ]; then
    # `|| true`: with only the Command Line Tools, xcodebuild exits non-zero,
    # and under set -e that would end the script before the guidance below.
    XCODE="$(xcodebuild -version 2>/dev/null | awk '/^Xcode/ {print $2}' || true)"
    if [ -z "$XCODE" ]; then
        die "Xcode not found. Qt $QT_VERSION needs Xcode $QT_MIN_XCODE+ (the Command Line
       Tools alone are not enough for Qt's SDK check)."
    fi
    if older_than "$XCODE" "$QT_MIN_XCODE"; then
        die "Qt $QT_VERSION needs Xcode $QT_MIN_XCODE+; this Mac has Xcode $XCODE. Qt's CMake
       stops at configure otherwise. Xcode $QT_MIN_XCODE needs macOS 14.5+."
    fi
    MACOS="$(sw_vers -productVersion)"
    if older_than "$MACOS" "$QT_MIN_MACOS"; then
        echo "WARNING: this Mac runs macOS $MACOS; Qt $QT_VERSION apps need $QT_MIN_MACOS+." >&2
        echo "         It can build here, but the result will not launch on this Mac." >&2
    fi
    echo "Xcode $XCODE — OK (Qt $QT_VERSION needs $QT_MIN_XCODE)"
fi

command -v python3 >/dev/null 2>&1 || die "python3 not found (needed for aqtinstall)."
command -v curl >/dev/null 2>&1 || die "curl not found."

mkdir -p "$CACHE_ROOT"
# 3 GB: ~2 GB installed plus the archives aqt holds while extracting.
FREE_KB="$(df -Pk "$CACHE_ROOT" | awk 'NR==2 {print $4}')"
if [ "${FREE_KB:-0}" -lt 3145728 ]; then
    die "need ~3 GB free under $CACHE_ROOT, have $((FREE_KB / 1024)) MB.
       Point AETHER_QT_CACHE at a roomier disk."
fi

# The repository must still serve the build the pin names. aqt asks for the
# version string only, so a republished Qt would otherwise install silently —
# the same trap the CI cache keys guard against with QT_PACKAGE_REVISION.
VER_TAG="qt6_$(echo "$QT_VERSION" | tr -d .)"
UPDATES_URL="https://download.qt.io/online/qtsdkrepository/$REPO_HOST/desktop/$VER_TAG/$VER_TAG/Updates.xml"
PKG="qt.qt6.$(echo "$QT_VERSION" | tr -d .).$AQT_ARCH"
# Fetched whole, then parsed: piping curl straight into an awk that exits at the
# first match kills curl with SIGPIPE, which pipefail reports as a failed fetch.
UPDATES_XML="$(curl -fsSL --max-time 60 "$UPDATES_URL")" \
    || die "could not read $UPDATES_URL (offline?)."
SERVED="$(printf '%s\n' "$UPDATES_XML" | tr -d '\r' | awk -v pkg="$PKG" '
    /<Name>/    { name = $0; sub(/.*<Name>/, "", name); sub(/<\/Name>.*/, "", name) }
    /<Version>/ && name == pkg && !done { v = $0; sub(/.*<Version>/, "", v); sub(/<\/Version>.*/, "", v); print v; done = 1 }')"
case "$SERVED" in
    *-"$QT_PACKAGE_REVISION") echo "Qt repository serves $SERVED — matches the pin" ;;
    "") die "$PKG is not listed in $UPDATES_URL." ;;
    *)  die "the Qt repository now serves $PKG $SERVED, but cmake/qt-pin.env pins
       revision $QT_PACKAGE_REVISION. Qt has republished $QT_VERSION; the pin needs a
       deliberate bump (and CI a re-run) before anyone builds against it." ;;
esac

# ── Install ──────────────────────────────────────────────────────────────
# Into a scratch directory first, renamed into place only once complete: an
# interrupted extraction must never leave a half-populated kit behind that a
# later configure would trust.
# The marker, not bin/aqt, says the venv is complete: an interrupted pip can
# leave the aqt entry point in place with its dependencies half-installed.
#
# macOS takes a py7zr floor instead of the exact pin: its stock python3 is
# Apple's 3.9, and py7zr 1.1+ needs 3.10+, so pip has no 1.1.3 to offer there.
# py7zr only extracts the archives — aqtinstall 3.3.0 asks for >=0.22.0, and the
# pin is CI's reproducibility, not a requirement on what lands — so 1.0.0 on
# 3.9 lays down the same kit, and a newer Python still gets 1.1.3 or later. The
# venv name says which rule built it, so neither reuses the other's cache.
if [ "$OS" = "Darwin" ]; then
    PY7ZR_REQ="py7zr>=1.0.0"; PY7ZR_TAG="floor1.0.0"
else
    PY7ZR_REQ="py7zr==$PY7ZR_VERSION"; PY7ZR_TAG="$PY7ZR_VERSION"
fi
VENV="$CACHE_ROOT/aqt-venv-$AQTINSTALL_VERSION-py7zr$PY7ZR_TAG"
if [ ! -f "$VENV/.complete" ]; then
    rm -rf "$VENV"
    if ! python3 -m venv "$VENV" >/dev/null 2>&1; then
        rm -rf "$VENV"
        die "python3 cannot create a virtual environment. On Debian, Ubuntu and
       Raspberry Pi OS install it with:  sudo apt install python3-venv"
    fi
    if ! "$VENV/bin/pip" install -q "aqtinstall==$AQTINSTALL_VERSION" "$PY7ZR_REQ"; then
        PYVER="$("$VENV/bin/python" -c 'import platform; print(platform.python_version())' 2>/dev/null || echo unknown)"
        rm -rf "$VENV"
        die "could not install aqtinstall $AQTINSTALL_VERSION and $PY7ZR_REQ into a venv
       built from python3 $PYVER ($(command -v python3)). py7zr 1.1+ needs
       Python 3.10+; put a newer python3 first on PATH and re-run."
    fi
    touch "$VENV/.complete"
fi

# Generations left by a run that was killed outright (no EXIT trap) are
# unpublished and unfinished; their owning PID is in the name. Remove only
# those whose owner is gone: a live one may be another checkout's install in
# progress.
mkdir -p "$GEN_ROOT"
for d in "$GEN_ROOT/$PIN_TAG"-*; do
    [ -d "$d" ] || continue
    name="${d##*/}"
    [ "$name" = "$CURRENT_GEN" ] && continue
    [ -f "$d/.aether-qt-stamp" ] && continue
    pid="${name##*-}"
    if ! kill -0 "$pid" 2>/dev/null; then
        echo "Removing an abandoned partial install: $name"
        rm -rf "$d"
    fi
done

# The new generation is built in its final location. Nothing points at it
# until the pointer is replaced below, so a failure here removes only itself.
NEW_GEN="$PIN_TAG-$(date +%s)-$$"
NEW_DIR="$GEN_ROOT/$NEW_GEN"
trap 'rm -rf "$NEW_DIR" "$POINTER.tmp.$$"' EXIT
echo "Installing Qt $QT_VERSION $AQT_ARCH ($QT_MODULES) — about 2 GB..."
# shellcheck disable=SC2086  # QT_MODULES is a deliberate word list
"$VENV/bin/aqt" install-qt "$AQT_HOST" desktop "$QT_VERSION" "$AQT_ARCH" \
    -m $QT_MODULES --outputdir "$NEW_DIR"

STAGED_PREFIX="$(prefix_of "$NEW_GEN")"
[ -x "$STAGED_PREFIX/bin/qmake" ] || die "aqt finished but left no qmake at $STAGED_PREFIX."
GOT="$("$STAGED_PREFIX/bin/qmake" -query QT_VERSION)"
[ "$GOT" = "$QT_VERSION" ] || die "aqt installed Qt $GOT, expected $QT_VERSION."
# qmake only proves qtbase landed; a partial install can still exit 0. Same
# assertion as the CI image (.github/docker/Dockerfile).
for m in $QT_MODULES; do
    case "$m" in
        qtmultimedia) pkg=Multimedia ;; qtwebsockets) pkg=WebSockets ;;
        qtserialport) pkg=SerialPort ;; qtshadertools) pkg=ShaderTools ;;
        *) continue ;;
    esac
    [ -f "$STAGED_PREFIX/lib/cmake/Qt6$pkg/Qt6${pkg}Config.cmake" ] ||
        die "aqt did not install Qt6$pkg ($m)."
done

echo "$STAMP_CONTENT" > "$NEW_DIR/.aether-qt-stamp"
# Publish: rename(2) of a regular file over another on the same filesystem is
# atomic, on GNU and BSD mv alike. Until this line the old generation is live;
# from it on, the new one is.
echo "$NEW_GEN" > "$POINTER.tmp.$$"
mv -f "$POINTER.tmp.$$" "$POINTER"
trap - EXIT
QT_PREFIX="$STAGED_PREFIX"
echo "Qt $QT_VERSION installed at $QT_PREFIX"
# The superseded generation stays: binaries built against it, in any checkout,
# keep launching. Builds pick up the new generation on their next configure
# (cmake/AetherQtPin.cmake drops cached entries for any generation that is not
# the live one). --prune reclaims the space when nothing needs it.
if [ -n "$CURRENT_GEN" ] && [ "$CURRENT_GEN" != "$NEW_GEN" ]; then
    echo "Previous generation $CURRENT_GEN kept; run with --prune to remove it."
fi

build_keychain

echo
echo "Done. CMake finds this Qt automatically:  cmake -B build -G Ninja"
echo "(-DAETHER_USE_PINNED_QT=OFF to build against another Qt instead.)"
