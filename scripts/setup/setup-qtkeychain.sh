#!/bin/bash
# setup-qtkeychain.sh — Build qtkeychain for Linux and macOS release packaging.
#
# Downloads the qtkeychain source, builds it from source against the Qt6
# installation in use (aqt-provided or system), and installs it into
# third_party/qtkeychain/ ready for CMake find_package(Qt6Keychain).
#
# Required for SmartLink credential persistence (saves the Auth0 refresh
# token in the system credential store so users are not prompted on every
# launch). The Linux AppImage previously shipped without it, so persistence
# silently compiled out (#3639).
#
# Building from source — rather than apt-installing qtkeychain-qt6-dev —
# guarantees the library matches the exact Qt the AppImage links (aqt Qt
# 6.8.3 on x86_64), avoiding an ABI mismatch with the distro's Qt.
#
# LIBSECRET_SUPPORT is OFF on purpose: that selects qtkeychain's pure
# Qt-D-Bus Secret Service backend, which talks to KDE Wallet (kwalletd) and
# GNOME Keyring over the session bus with no extra native runtime deps to
# bundle — only Qt6 D-Bus, which linuxdeploy already carries. It has no effect
# on macOS, where qtkeychain always uses the native Keychain backend.
#
# Requires: cmake, ninja, a C++ compiler, git, and a discoverable Qt6.
#
# MACOS_DEPLOYMENT_TARGET (optional, macOS only) must be set to the SAME value
# the app is built with. CMake otherwise leaves CMAKE_OSX_DEPLOYMENT_TARGET
# empty and clang stamps the *runner's* OS into LC_BUILD_VERSION — a dylib with
# minos 15.0 inside a bundle that advertises 14.0, which dyld refuses to load on
# macOS 14. The library is copied into Contents/Frameworks (CMakeLists.txt,
# APPLE branch of the Qt6Keychain block), so it ships with that stamp. The
# `${VAR:+…}` form is safe under `set -u` and passes nothing when unset, leaving
# Linux exactly as it was.
#
# Usage: ./setup-qtkeychain.sh

set -euo pipefail

QTKEYCHAIN_VERSION="0.16.0"
QTKEYCHAIN_REPO="https://github.com/frankosterfeld/qtkeychain.git"
# Immutable commit the 0.16.0 tag points to (#3665). Pinning the commit, not
# just the tag, defends against a moved/retagged release. Bump with the version.
QTKEYCHAIN_COMMIT="aa6da344e1a20b9194e12bace3665caeea6b6304"
OUT_DIR="third_party/qtkeychain"

# ── Already set up? (lets CI cache third_party/qtkeychain) ───────────────
# The stamp carries the deployment target as well, so switching targets rebuilds
# instead of reusing a dylib built for a different floor — the reuse would be
# invisible until the DMG failed to launch on the older OS.
STAMP="$OUT_DIR/.build-stamp"
STAMP_CONTENT="version=$QTKEYCHAIN_VERSION target=${MACOS_DEPLOYMENT_TARGET:-host}"
if [ -f "$OUT_DIR/lib/cmake/Qt6Keychain/Qt6KeychainConfig.cmake" ] &&
   [ -f "$STAMP" ] && [ "$(cat "$STAMP")" = "$STAMP_CONTENT" ]; then
    echo "qtkeychain already set up in $OUT_DIR ($STAMP_CONTENT)"
    exit 0
fi

# ── Locate Qt6 ───────────────────────────────────────────────────────────
# CI sets CMAKE_PREFIX_PATH to the aqt Qt; fall back to Qt6_DIR or qmake.
QT_PREFIX="${CMAKE_PREFIX_PATH:-}"
if [ -z "$QT_PREFIX" ] && [ -n "${Qt6_DIR:-}" ]; then
    QT_PREFIX="$(cd "$Qt6_DIR/../../.." && pwd)"
fi
if [ -z "$QT_PREFIX" ]; then
    if command -v qmake6 >/dev/null 2>&1; then
        QT_PREFIX="$(qmake6 -query QT_INSTALL_PREFIX)"
    elif command -v qmake >/dev/null 2>&1; then
        QT_PREFIX="$(qmake -query QT_INSTALL_PREFIX)"
    fi
fi
if [ -z "$QT_PREFIX" ]; then
    echo "ERROR: Qt6 not found. Set CMAKE_PREFIX_PATH or Qt6_DIR, or install qmake6." >&2
    exit 1
fi
echo "Using Qt from: $QT_PREFIX"

OUT_DIR_ABS="$(mkdir -p "$OUT_DIR" && cd "$OUT_DIR" && pwd)"
BUILD_DIR="$(dirname "$OUT_DIR_ABS")/qtkeychain-build"
SRC_DIR="$(dirname "$OUT_DIR_ABS")/qtkeychain-src"

# ── Fetch source ─────────────────────────────────────────────────────────
rm -rf "$SRC_DIR" "$BUILD_DIR"
echo "Cloning qtkeychain $QTKEYCHAIN_VERSION..."
git clone --depth 1 --branch "$QTKEYCHAIN_VERSION" "$QTKEYCHAIN_REPO" "$SRC_DIR"

# Verify the tag resolved to the pinned commit (supply-chain hardening #3665).
ACTUAL_COMMIT="$(git -C "$SRC_DIR" rev-parse HEAD)"
if [ "$ACTUAL_COMMIT" != "$QTKEYCHAIN_COMMIT" ]; then
    echo "ERROR: qtkeychain $QTKEYCHAIN_VERSION resolved to $ACTUAL_COMMIT," >&2
    echo "       expected $QTKEYCHAIN_COMMIT (tag moved or tampering)." >&2
    exit 1
fi
echo "Verified qtkeychain commit $QTKEYCHAIN_COMMIT"

# ── Build + install ──────────────────────────────────────────────────────
# On macOS the deployment target has to be passed through, not left to default.
# Without it clang targets the BUILD HOST, so a macos-15 runner produces a dylib
# with LC_BUILD_VERSION minos 15.0 — and since CMakeLists.txt copies
# libqt6keychain.1.dylib into Contents/Frameworks, dyld then refuses to load it
# on anything older and the app dies at launch rather than degrading. The bundle
# floor is the MAX minos across everything staged, so one unpinned dependency
# silently overrides the deployment target of the whole DMG. Same reasoning as
# scripts/setup/setup-macos-deps.sh; asserted after macdeployqt by
# scripts/build/assert-macos-bundle.sh.
# Warn rather than fail when it is unset: a developer building locally wants the
# host default and has no bundle to break. The release path always sets it, and
# assert-macos-bundle.sh fails the DMG if it ever stops doing so — so the check
# that matters is downstream, and this stays usable off CI.
QTKEYCHAIN_OSX_TARGET=()
if [ "$(uname -s)" = "Darwin" ]; then
    if [ -n "${MACOS_DEPLOYMENT_TARGET:-}" ]; then
        QTKEYCHAIN_OSX_TARGET=(-DCMAKE_OSX_DEPLOYMENT_TARGET="$MACOS_DEPLOYMENT_TARGET")
        echo "Targeting macOS $MACOS_DEPLOYMENT_TARGET"
    else
        echo "WARNING: MACOS_DEPLOYMENT_TARGET is unset — qtkeychain will inherit this" >&2
        echo "         host's macOS version. Fine locally; NOT fine for a release DMG." >&2
    fi
fi

echo "Building qtkeychain $QTKEYCHAIN_VERSION from source..."
cmake -B "$BUILD_DIR" -S "$SRC_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    "${QTKEYCHAIN_OSX_TARGET[@]}" \
    -DBUILD_WITH_QT6=ON \
    -DBUILD_SHARED_LIBS=ON \
    -DBUILD_TRANSLATIONS=OFF \
    -DLIBSECRET_SUPPORT=OFF \
    -DCMAKE_PREFIX_PATH="$QT_PREFIX" \
    -DCMAKE_INSTALL_PREFIX="$OUT_DIR_ABS" \
    -DCMAKE_INSTALL_LIBDIR=lib \
    ${MACOS_DEPLOYMENT_TARGET:+-DCMAKE_OSX_DEPLOYMENT_TARGET="$MACOS_DEPLOYMENT_TARGET"}
# nproc is GNU coreutils and absent on macOS, where this script now also runs
# (the Apple Silicon DMG builds qtkeychain against its aqt Qt for the same ABI
# reason as Linux). sysctl is the BSD equivalent. Fall back to 1 rather than
# letting the substitution come out empty — a bare `-j` means "unlimited" and
# would fork-bomb the runner.
JOBS="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 1)"
cmake --build "$BUILD_DIR" -j"$JOBS"
cmake --install "$BUILD_DIR"

# ── Cleanup ──────────────────────────────────────────────────────────────
rm -rf "$SRC_DIR" "$BUILD_DIR"
echo "$STAMP_CONTENT" > "$STAMP"

echo "qtkeychain ready in $OUT_DIR_ABS"
echo "  cmake config: $OUT_DIR_ABS/lib/cmake/Qt6Keychain/Qt6KeychainConfig.cmake"
