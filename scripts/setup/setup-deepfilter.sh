#!/bin/bash
# setup-deepfilter.sh — Build DeepFilterNet3 libdf for DFNR noise reduction.
#
# Downloads the DeepFilterNet source, builds the C API library using Cargo,
# and copies the output to third_party/deepfilter/ ready for CMake.
#
# Requires: Rust toolchain (cargo), git
# Install Rust: curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
#
# Usage: ./setup-deepfilter.sh

set -euo pipefail

DFNR_COMMIT="d375b2d8309e0935d165700c91da9de862a99c31"
DFNR_REPO="https://github.com/Rikorose/DeepFilterNet.git"
OUT_DIR="third_party/deepfilter"
MODEL_NAME="DeepFilterNet3_onnx.tar.gz"

# ── Detect platform ──────────────────────────────────────────────────────
OS=$(uname -s)
ARCH=$(uname -m)

case "$OS" in
    Linux)
        case "$ARCH" in
            x86_64)  PLATFORM="linux-x86_64";  RUST_TARGET="x86_64-unknown-linux-gnu" ;;
            aarch64) PLATFORM="linux-aarch64";  RUST_TARGET="aarch64-unknown-linux-gnu" ;;
            *)       echo "Unsupported architecture: $ARCH"; exit 1 ;;
        esac
        LIB_NAME="libdeepfilter.a"
        ;;
    Darwin)
        case "$ARCH" in
            arm64)   PLATFORM="darwin-arm64";   RUST_TARGET="aarch64-apple-darwin" ;;
            x86_64)  PLATFORM="darwin-x86_64";  RUST_TARGET="x86_64-apple-darwin" ;;
            *)       echo "Unsupported architecture: $ARCH"; exit 1 ;;
        esac
        LIB_NAME="libdeepfilter.a"
        ;;
    *)
        echo "Unsupported OS: $OS (use setup-deepfilter.ps1 for Windows)"
        exit 1
        ;;
esac

LIB_DIR="$OUT_DIR/lib/$PLATFORM"

# ── Check if already built ───────────────────────────────────────────────
if [ -f "$LIB_DIR/$LIB_NAME" ] && [ -f "$OUT_DIR/models/$MODEL_NAME" ]; then
    echo "DeepFilterNet3 already set up in $LIB_DIR"
    exit 0
fi

# ── Try downloading pre-built binary ────────────────────────────────────
# Pre-built libraries are hosted on the GitHub Release tagged 'dfnr-libs'.
# Override the repo with DFNR_RELEASE_REPO if building from a fork.
RELEASE_REPO="${DFNR_RELEASE_REPO:-ten9876/AetherSDR}"
RELEASE_TAG="dfnr-libs"
TARBALL="libdeepfilter-${PLATFORM}.tar.gz"
DOWNLOAD_URL="https://github.com/${RELEASE_REPO}/releases/download/${RELEASE_TAG}/${TARBALL}"

echo "Trying pre-built binary from $DOWNLOAD_URL ..."
if curl -fsSL --retry 2 --connect-timeout 10 -o "/tmp/$TARBALL" "$DOWNLOAD_URL" 2>/dev/null; then
    mkdir -p "$OUT_DIR"
    tar xzf "/tmp/$TARBALL" -C "$OUT_DIR"
    rm -f "/tmp/$TARBALL"
    if [ -f "$LIB_DIR/$LIB_NAME" ]; then
        echo "DeepFilterNet3 ready (pre-built) in $LIB_DIR"
        exit 0
    fi
    echo "Download succeeded but library not found — falling back to source build"
else
    echo "Pre-built binary not available — falling back to source build"
fi

# ── Check prerequisites ─────────────────────────────────────────────────
if ! command -v cargo &>/dev/null; then
    echo "ERROR: Rust toolchain not found. Install with:"
    echo "  curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh"
    exit 1
fi

if ! command -v cargo-cbuild &>/dev/null; then
    echo "Installing cargo-c..."
    cargo install cargo-c
fi

# ── Clone and build ──────────────────────────────────────────────────────
TMPDIR=$(mktemp -d)
trap "rm -rf '$TMPDIR'" EXIT

echo "Cloning DeepFilterNet at $DFNR_COMMIT..."
git clone --depth 50 "$DFNR_REPO" "$TMPDIR/DeepFilterNet"
(cd "$TMPDIR/DeepFilterNet" && git checkout "$DFNR_COMMIT")

echo "Building libdf for $PLATFORM ($RUST_TARGET)..."
(cd "$TMPDIR/DeepFilterNet" && \
    cargo cbuild --release -p deep_filter --features capi --target "$RUST_TARGET")

# ── Locate build outputs ────────────────────────────────────────────────
BUILD_DIR="$TMPDIR/DeepFilterNet/target/$RUST_TARGET/release"

if [ ! -f "$BUILD_DIR/$LIB_NAME" ]; then
    echo "ERROR: Build succeeded but $LIB_NAME not found in $BUILD_DIR"
    ls -la "$BUILD_DIR"/libdeepfilter* 2>/dev/null || true
    exit 1
fi

# ── Copy outputs ─────────────────────────────────────────────────────────
mkdir -p "$LIB_DIR"
mkdir -p "$OUT_DIR/include"
mkdir -p "$OUT_DIR/models"

cp "$BUILD_DIR/$LIB_NAME" "$LIB_DIR/"
echo "  Library: $LIB_DIR/$LIB_NAME ($(du -h "$LIB_DIR/$LIB_NAME" | cut -f1))"

# Header
HEADER="$BUILD_DIR/include/deep_filter/deep_filter.h"
if [ -f "$HEADER" ]; then
    cp "$HEADER" "$OUT_DIR/include/"
fi

# Model
MODEL_SRC="$TMPDIR/DeepFilterNet/models/$MODEL_NAME"
if [ -f "$MODEL_SRC" ]; then
    cp "$MODEL_SRC" "$OUT_DIR/models/"
    echo "  Model:   $OUT_DIR/models/$MODEL_NAME"
fi

# Commit hash
echo "$DFNR_COMMIT" > "$OUT_DIR/COMMIT"

echo "DeepFilterNet3 ready in $LIB_DIR"
