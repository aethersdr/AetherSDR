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

# macOS: drop sherpa's bundled ONNX Runtime and use the upstream one instead.
#
# k2-fsa build theirs on whatever macOS their runner happens to be, so it lands
# at LC_BUILD_VERSION minos 15.5 on BOTH slices of the universal2 archive.
# macdeployqt stages it into Contents/Frameworks and the app links it at load
# time, so that stamp becomes the DMG's real minimum macOS regardless of
# CMAKE_OSX_DEPLOYMENT_TARGET — the app simply will not start below it
# (#4532). Microsoft's own 1.27.0 build is 14.0, which is the floor the Apple
# Silicon DMG already declares.
#
# The two are interchangeable here: the setup scripts pin the SAME ORT version
# on purpose, so sherpa's libsherpa-onnx-c-api.dylib resolves its
# @rpath/libonnxruntime.1.27.0.dylib against the upstream copy with the same
# install name and the same C API. Removing sherpa's copy is what makes the
# bundle carry exactly one runtime rather than silently preferring the higher
# one — see the APPLE branch of the sherpa/ORT sharing block in CMakeLists.txt.
#
# Upstream publishes no x86_64 macOS build at all, so there is nothing to fall
# back to on Intel; macos-dmg.yml does not stage sherpa on that leg for exactly
# this reason, and this guard makes a mistake there loud instead of shipping a
# DMG that needs macOS 15.5.
if [ "${os}" = "Darwin" ]; then
    if [ ! -f "${REPO_ROOT}/third_party/onnxruntime/lib/libonnxruntime.dylib" ]; then
        echo "ERROR: sherpa-onnx on macOS requires the upstream ONNX Runtime to be" >&2
        echo "       staged first (scripts/setup/setup-onnxruntime.sh). sherpa's own" >&2
        echo "       bundled runtime is built at minos 15.5 and would become the" >&2
        echo "       bundle's minimum macOS. Upstream has no x86_64 macOS build, so" >&2
        echo "       Intel Macs cannot use the sherpa backend at all." >&2
        exit 1
    fi
    echo "Dropping sherpa's bundled ONNX Runtime (minos 15.5); using the upstream 14.0 build."
    rm -f "${OUT_DIR}"/lib/libonnxruntime*.dylib
fi

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
