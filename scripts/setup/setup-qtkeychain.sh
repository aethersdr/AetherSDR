#!/bin/bash
# setup-qtkeychain.sh — Build qtkeychain for Linux release packaging.
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
# bundle — only Qt6 D-Bus, which linuxdeploy already carries.
#
# Requires: cmake, ninja, a C++ compiler, git, and a discoverable Qt6.
#
# Usage: ./setup-qtkeychain.sh

set -euo pipefail

QTKEYCHAIN_VERSION="0.16.0"
QTKEYCHAIN_REPO="https://github.com/frankosterfeld/qtkeychain.git"
OUT_DIR="third_party/qtkeychain"

# ── Already set up? (lets CI cache third_party/qtkeychain) ───────────────
if [ -f "$OUT_DIR/lib/cmake/Qt6Keychain/Qt6KeychainConfig.cmake" ]; then
    echo "qtkeychain already set up in $OUT_DIR"
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

# ── Build + install ──────────────────────────────────────────────────────
echo "Building qtkeychain $QTKEYCHAIN_VERSION from source..."
cmake -B "$BUILD_DIR" -S "$SRC_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_WITH_QT6=ON \
    -DBUILD_SHARED_LIBS=ON \
    -DBUILD_TRANSLATIONS=OFF \
    -DLIBSECRET_SUPPORT=OFF \
    -DCMAKE_PREFIX_PATH="$QT_PREFIX" \
    -DCMAKE_INSTALL_PREFIX="$OUT_DIR_ABS" \
    -DCMAKE_INSTALL_LIBDIR=lib
cmake --build "$BUILD_DIR" -j"$(nproc)"
cmake --install "$BUILD_DIR"

# ── Cleanup ──────────────────────────────────────────────────────────────
rm -rf "$SRC_DIR" "$BUILD_DIR"

echo "qtkeychain ready in $OUT_DIR_ABS"
echo "  cmake config: $OUT_DIR_ABS/lib/cmake/Qt6Keychain/Qt6KeychainConfig.cmake"
