#!/bin/bash
# setup-onnxruntime.sh — stage a prebuilt ONNX Runtime for Linux/macOS.
#
# Downloads the official ONNX Runtime prebuilt for the host OS/arch and places
# headers + shared library under third_party/onnxruntime/ ready for CMake (which
# checks that dir first). Powers the CNN signal classifier AND the ASR ONNX
# features (Silero VAD + speaker labeling). Release builds run this so those
# ship enabled; pass -DREQUIRE_ASR_ONNX=ON to fail the build if it's missing.
#
# Requires: curl, tar. Usage: ./setup-onnxruntime.sh
#
# Keep ORT_VERSION in sync with scripts/setup/setup-onnxruntime.ps1 (Windows).

set -euo pipefail

# shellcheck source=scripts/setup/_verify_sha256.sh
source "$(dirname "${BASH_SOURCE[0]}")/_verify_sha256.sh"

# Pinned to the ONNX Runtime that sherpa-onnx bundles (so a build with both
# shares one runtime — no symbol-version clash). Keep in sync with
# setup-onnxruntime.ps1 and setup-sherpa-onnx.sh.
ORT_VERSION="1.27.0"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT_DIR="${REPO_ROOT}/third_party/onnxruntime"

if [ -f "${OUT_DIR}/include/onnxruntime_cxx_api.h" ]; then
    echo "ONNX Runtime already staged in ${OUT_DIR}"
    exit 0
fi

# Select the release asset + its pinned SHA-256 for this host.
os="$(uname -s)"
arch="$(uname -m)"
case "${os}-${arch}" in
    Linux-x86_64)        asset="linux-x64"     sha="547e40a48f1fe73e3f812d7c88a948612c23f896b91e4e2ee1e232d7b468246f" ;;
    Linux-aarch64|Linux-arm64) asset="linux-aarch64" sha="3e4d83ac06924a32a07b6d7f91ce6f852876153fc0bbdf931bf517a140bfbe48" ;;
    Darwin-arm64)        asset="osx-arm64"     sha="545e81c58152353acb0d1e8bd6ce4b62f830c0961f5b3acfedc790ffd76e477a" ;;
    Darwin-x86_64)
        echo "ERROR: ONNX Runtime ${ORT_VERSION} has no Intel-macOS prebuilt (arm64 only). Stage" >&2
        echo "sherpa-onnx (universal2, bundles the same runtime) or build ONNX Runtime from source." >&2
        exit 1 ;;
    *)
        echo "ERROR: unsupported host ${os}-${arch}. Install onnxruntime manually or extend this script." >&2
        exit 1 ;;
esac

pkg="onnxruntime-${asset}-${ORT_VERSION}"
url="https://github.com/microsoft/onnxruntime/releases/download/v${ORT_VERSION}/${pkg}.tgz"
tgz="${REPO_ROOT}/third_party/${pkg}.tgz"

mkdir -p "${REPO_ROOT}/third_party"
if [ ! -f "${tgz}" ]; then
    echo "Downloading ${pkg}.tgz ..."
    curl -fSL --retry 3 -o "${tgz}" "${url}"
fi
verify_sha256 "${tgz}" "${sha}"

echo "Extracting ..."
tmp="${REPO_ROOT}/third_party/onnxruntime-tmp"
rm -rf "${tmp}" "${OUT_DIR}"
mkdir -p "${tmp}"
tar xzf "${tgz}" -C "${tmp}"

src="${tmp}/${pkg}"
[ -d "${src}" ] || src="$(find "${tmp}" -maxdepth 1 -type d -name 'onnxruntime-*' | head -1)"
if [ ! -f "${src}/include/onnxruntime_cxx_api.h" ]; then
    echo "ERROR: unexpected archive layout under ${tmp}" >&2
    exit 1
fi

mkdir -p "${OUT_DIR}"
cp -a "${src}/include" "${OUT_DIR}/include"
cp -a "${src}/lib" "${OUT_DIR}/lib"
rm -rf "${tmp}"

echo "ONNX Runtime ${ORT_VERSION} (${asset}) staged in ${OUT_DIR}"
