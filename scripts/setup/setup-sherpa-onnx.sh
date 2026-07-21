#!/bin/bash
# setup-sherpa-onnx.sh — stage a prebuilt sherpa-onnx (C API) for Linux/macOS.
#
# Downloads the official sherpa-onnx shared-library release for the host and
# places the C-API header + shared libs under third_party/sherpa-onnx/ ready for
# CMake (which checks that dir and sets HAVE_SHERPA). Enables the non-whisper
# ASR backend (offline sherpa-onnx models). sherpa-onnx bundles its own ONNX
# Runtime, so this is self-contained.
#
# The "-lib" release ships binaries only; the C-API header is fetched from source
# at the matching tag. Requires: curl, tar.
#
# k2-fsa publishes no linux-aarch64 shared-lib prebuilt, so that one is built by
# AetherSDR's own CI and hosted on the `sherpa-onnx-libs` release (see below).

set -euo pipefail

# shellcheck source=scripts/setup/_verify_sha256.sh
source "$(dirname "${BASH_SOURCE[0]}")/_verify_sha256.sh"

SHERPA_VERSION="1.13.4"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT_DIR="${REPO_ROOT}/third_party/sherpa-onnx"

if [ -f "${OUT_DIR}/include/sherpa-onnx/c-api/c-api.h" ]; then
    echo "sherpa-onnx already staged in ${OUT_DIR}"
    exit 0
fi

os="$(uname -s)"
arch="$(uname -m)"
# Most platforms use k2-fsa's upstream prebuilt; linux-aarch64 (which k2-fsa does
# not publish) uses AetherSDR's own build, hosted on the `sherpa-onnx-libs`
# release and produced by .github/workflows/build-sherpa-onnx-aarch64.yml.
base_url="https://github.com/k2-fsa/sherpa-onnx/releases/download/v${SHERPA_VERSION}"
case "${os}-${arch}" in
    Linux-x86_64) asset="linux-x64"      sha="8a2c6d5f8d04e651c90e71ba3ee8b08dedb2b741ff5f316e12aa629423f91c9f" ;;
    Linux-aarch64|Linux-arm64)
        asset="linux-aarch64"
        sha="b662151bbb55451a6780c131d6c797f61cb43eaea7f3fdd1ff68f3ce47e4aaea"
        base_url="https://github.com/aethersdr/AetherSDR/releases/download/sherpa-onnx-libs" ;;
    Darwin-*)     asset="osx-universal2" sha="84fe9103dff74f7688cd5b6348f8abc8dd6842a46d149bf0bfdd185e841f1594" ;;
    *)
        echo "ERROR: no sherpa-onnx shared-lib prebuilt for ${os}-${arch}. Build from source." >&2
        exit 1 ;;
esac

pkg="sherpa-onnx-v${SHERPA_VERSION}-${asset}-shared-no-tts-lib"
url="${base_url}/${pkg}.tar.bz2"
tar_bz2="${REPO_ROOT}/third_party/${pkg}.tar.bz2"

mkdir -p "${REPO_ROOT}/third_party"
if [ ! -f "${tar_bz2}" ]; then
    echo "Downloading ${pkg}.tar.bz2 ..."
    curl -fSL --retry 3 -o "${tar_bz2}" "${url}"
fi
verify_sha256 "${tar_bz2}" "${sha}"

echo "Extracting libs ..."
tmp="${REPO_ROOT}/third_party/_sherpa_extract"
rm -rf "${tmp}" "${OUT_DIR}"
mkdir -p "${tmp}" "${OUT_DIR}/lib" "${OUT_DIR}/include/sherpa-onnx/c-api"
tar xjf "${tar_bz2}" -C "${tmp}"
src="$(find "${tmp}" -mindepth 1 -maxdepth 1 -type d -name 'sherpa-onnx-*' | head -1)"
if [ -z "${src}" ] || [ ! -d "${src}/lib" ]; then
    echo "ERROR: unexpected archive layout under ${tmp}" >&2
    exit 1
fi
cp -a "${src}/lib/." "${OUT_DIR}/lib/"   # all shared libs (c-api, cxx-api, onnxruntime)
rm -rf "${tmp}"

# On macOS the k2-fsa prebuilt ships ad-hoc signed dylibs whose signature is
# invalid on arrival (upstream strip/repack breaks the seal). dyld then
# SIGKILLs the app at load time with "Code Signature Invalid". Re-sign ad-hoc
# so the seal matches the on-disk bytes.
if [ "${os}" = "Darwin" ]; then
    echo "Re-signing dylibs (ad-hoc) ..."
    codesign --force --sign - "${OUT_DIR}"/lib/*.dylib
fi

echo "Fetching C-API header ..."
curl -fSL --retry 3 -o "${OUT_DIR}/include/sherpa-onnx/c-api/c-api.h" \
    "https://raw.githubusercontent.com/k2-fsa/sherpa-onnx/v${SHERPA_VERSION}/sherpa-onnx/c-api/c-api.h"

echo "sherpa-onnx ${SHERPA_VERSION} (${asset}) staged in ${OUT_DIR}"
