#!/bin/bash
# setup-macos-deps.sh — Build fftw, portaudio and hidapi from source at a
# pinned macOS deployment target, for the release DMG.
#
# ── Why this exists instead of `brew install fftw portaudio hidapi` ──────
#
# A Homebrew bottle is compiled for the macOS it was built on, and Homebrew
# does not publish a bottle for every macOS it will happily install onto. The
# newest bottle each DMG runner resolves to, and what it actually carries:
#
#     macos-15        (arm64)  -> arm64_sequoia  -> LC_BUILD_VERSION minos 15.0
#     macos-15-intel  (x86_64) -> sonoma         -> LC_BUILD_VERSION minos 14.0
#
# (fftw has no x86_64 sequoia/tahoe bottle at all — sonoma is the newest Intel
# tag it publishes. Checked against formulae.brew.sh, and the minos values above
# were read out of the downloaded bottles with otool, not inferred.)
#
# macdeployqt stages those dylibs into Contents/Frameworks, and dyld refuses to
# load a dylib whose minos is newer than the running OS. So the DMG's real floor
# was the runner image's OS version, NOT CMAKE_OSX_DEPLOYMENT_TARGET: the Apple
# Silicon DMG declared 14.0 and could only launch on 15, and the Intel DMG
# declared 13.0 and could only launch on 14. Nothing reported it, because the
# app's own Mach-O header was correct and no step ever looked at the bundle.
#
# That is the mechanism behind #4532 ("AetherSDR.app crashes on launch on macOS
# Ventura 13.7.8"), and it is the same diagnosis #802 made — it just fixed Qt and
# left the other three linked libraries on Homebrew.
#
# Building them here pins all three to the same floor the app declares, so the
# deployment target becomes a statement about the DMG rather than about whichever
# runner image GitHub happened to give us. scripts/build/assert-macos-deployment-floor.sh
# proves it after macdeployqt rather than trusting this script.
#
# ── Cost ────────────────────────────────────────────────────────────────
#
# ~8 min cold, dominated by the two FFTW builds (it is compiled twice: once
# double for WDSP, once single for libspecbleach). The output tree is cached on
# (runner, deployment target, this script), so it is a one-off per Qt/dep bump
# rather than per release — the same deal third_party/qtkeychain already has.
#
# Requires: curl, cmake, ninja, make, a C compiler, and the autotools already
# installed for the DMG build (autoconf/automake/libtool). All three tarballs
# ship a pre-generated ./configure, so nothing here runs autoreconf.
#
# Usage: MACOS_DEPLOYMENT_TARGET=12.0 ./scripts/setup/setup-macos-deps.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=scripts/setup/_verify_sha256.sh
. "$SCRIPT_DIR/_verify_sha256.sh"

if [ "$(uname -s)" != "Darwin" ]; then
    echo "ERROR: setup-macos-deps.sh is macOS-only (got $(uname -s))." >&2
    exit 1
fi

TARGET="${MACOS_DEPLOYMENT_TARGET:-}"
if [ -z "$TARGET" ]; then
    echo "ERROR: MACOS_DEPLOYMENT_TARGET is required — this script exists to pin" >&2
    echo "       the deployment target, so defaulting it would defeat the point." >&2
    exit 1
fi

# Pinned upstream sources. These are the same releases and the same checksums
# Homebrew builds from (formulae.brew.sh urls.stable.checksum) — the change here
# is the deployment target, not the software. Bump versions and hashes together.
FFTW_VERSION="3.3.11"
FFTW_URL="https://fftw.org/fftw-${FFTW_VERSION}.tar.gz"
FFTW_SHA256="5630c24cdeb33b131612f7eb4b1a9934234754f9f388ff8617458d0be6f239a1"

PORTAUDIO_VERSION="19.7.0"
PORTAUDIO_URL="https://files.portaudio.com/archives/pa_stable_v190700_20210406.tgz"
PORTAUDIO_SHA256="47efbf42c77c19a05d22e627d42873e991ec0c1357219c0d74ce6a2948cb2def"

HIDAPI_VERSION="0.15.0"
HIDAPI_URL="https://github.com/libusb/hidapi/archive/refs/tags/hidapi-${HIDAPI_VERSION}.tar.gz"
HIDAPI_SHA256="5d84dec684c27b97b921d2f3b73218cb773cf4ea915caee317ac8fc73cef8136"

OUT_DIR="third_party/macos-deps"
PREFIX="$(pwd)/$OUT_DIR"
WORK_DIR="$(pwd)/.macos-deps-build"
STAMP="$OUT_DIR/.build-stamp"
STAMP_CONTENT="target=$TARGET fftw=$FFTW_VERSION portaudio=$PORTAUDIO_VERSION hidapi=$HIDAPI_VERSION arch=$(uname -m)"

# ── Already set up? (lets CI cache third_party/macos-deps) ───────────────
# The stamp carries the deployment target, so a local rebuild at a different
# floor rebuilds rather than silently reusing a tree built for another one —
# which is the exact failure mode this script exists to remove.
if [ -f "$STAMP" ] && [ "$(cat "$STAMP")" = "$STAMP_CONTENT" ]; then
    echo "macOS deps already built for $STAMP_CONTENT"
    exit 0
fi

rm -rf "$OUT_DIR" "$WORK_DIR"
mkdir -p "$PREFIX" "$WORK_DIR"

# Autotools reads the deployment target from the environment; the CMake build
# below gets it as a -D. Both end up as LC_BUILD_VERSION minos on the dylib.
export MACOSX_DEPLOYMENT_TARGET="$TARGET"

fetch() {  # fetch <url> <sha256> <output>
    local url="$1" sha="$2" out="$3"
    echo "Downloading $(basename "$out")..."
    curl -fsSL --retry 3 --connect-timeout 30 -o "$WORK_DIR/$out" "$url"
    verify_sha256 "$WORK_DIR/$out" "$sha"
}

fetch "$FFTW_URL"      "$FFTW_SHA256"      fftw.tar.gz
fetch "$PORTAUDIO_URL" "$PORTAUDIO_SHA256" portaudio.tgz
fetch "$HIDAPI_URL"    "$HIDAPI_SHA256"    hidapi.tar.gz

JOBS="$(sysctl -n hw.ncpu 2>/dev/null || echo 1)"

# ── FFTW ────────────────────────────────────────────────────────────────
# Built twice: WDSP needs double (-lfftw3), libspecbleach needs single
# (-lfftw3f). FFTW selects its SIMD codelets at RUNTIME via cpuid, so enabling
# AVX/AVX2 here does not raise the CPU floor — a 2017 iMac takes the SSE2 path
# out of the same binary. That property is why these flags are safe to pass on
# a build whose whole purpose is reaching older hardware.
case "$(uname -m)" in
    arm64)  FFTW_SIMD=(--enable-neon) ;;
    x86_64) FFTW_SIMD=(--enable-sse2 --enable-avx --enable-avx2) ;;
    *)      FFTW_SIMD=() ;;
esac

tar xzf "$WORK_DIR/fftw.tar.gz" -C "$WORK_DIR"
for precision in double single; do
    build="$WORK_DIR/fftw-build-$precision"
    mkdir -p "$build"
    float_flag=()
    [ "$precision" = "single" ] && float_flag=(--enable-float)
    echo "Building FFTW $FFTW_VERSION ($precision) for macOS $TARGET..."
    # ${arr[@]+"${arr[@]}"}, not "${arr[@]}": macOS ships bash 3.2, where
    # expanding an EMPTY array under `set -u` is an unbound-variable error.
    # bash 4.4 fixed that, but the runner never sees 4.4. float_flag is empty on
    # the first iteration (double), so the plain form killed this script four
    # seconds in — before anything it builds, and before the deployment-floor
    # assert that is the whole point of the step. FFTW_SIMD takes the same guard:
    # it is empty on any arch that hits the `*)` case above.
    ( cd "$build" && "$WORK_DIR/fftw-$FFTW_VERSION/configure" \
        --prefix="$PREFIX" \
        --enable-shared --disable-static \
        --disable-fortran --disable-doc \
        ${float_flag[@]+"${float_flag[@]}"} \
        ${FFTW_SIMD[@]+"${FFTW_SIMD[@]}"} >/dev/null )
    make -C "$build" -j"$JOBS" >/dev/null
    make -C "$build" install >/dev/null
done

# ── PortAudio ───────────────────────────────────────────────────────────
# --disable-mac-universal: portaudio's configure otherwise forces its own
# -arch flags, which fight the runner's native arch and drop the deployment
# target we just exported. Homebrew disables it for the same reason.
tar xzf "$WORK_DIR/portaudio.tgz" -C "$WORK_DIR"
echo "Building PortAudio $PORTAUDIO_VERSION for macOS $TARGET..."
( cd "$WORK_DIR/portaudio" && ./configure \
    --prefix="$PREFIX" \
    --enable-shared --disable-static \
    --disable-mac-universal >/dev/null )
make -C "$WORK_DIR/portaudio" -j"$JOBS" >/dev/null
make -C "$WORK_DIR/portaudio" install >/dev/null

# ── hidapi ──────────────────────────────────────────────────────────────
# CMAKE_INSTALL_NAME_DIR pins an absolute install_name instead of CMake's
# default @rpath. macdeployqt resolves an absolute path the same way it always
# resolved Homebrew's; an @rpath name would depend on an rpath entry pointing
# into the build tree and would not survive the copy into the bundle.
tar xzf "$WORK_DIR/hidapi.tar.gz" -C "$WORK_DIR"
echo "Building hidapi $HIDAPI_VERSION for macOS $TARGET..."
cmake -B "$WORK_DIR/hidapi-build" -S "$WORK_DIR/hidapi-hidapi-$HIDAPI_VERSION" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="$TARGET" \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DCMAKE_INSTALL_LIBDIR=lib \
    -DCMAKE_INSTALL_NAME_DIR="$PREFIX/lib" \
    -DBUILD_SHARED_LIBS=ON \
    -DHIDAPI_BUILD_HIDTEST=OFF >/dev/null
cmake --build "$WORK_DIR/hidapi-build" -j"$JOBS" >/dev/null
cmake --install "$WORK_DIR/hidapi-build" >/dev/null

# ── Verify the whole point of the exercise ──────────────────────────────
# A build that silently ignored MACOSX_DEPLOYMENT_TARGET would look exactly like
# a successful one until a user on the older OS tried to launch the DMG, which is
# how this went unnoticed for two releases. Check the headers we came for.
fail=0
for lib in "$PREFIX"/lib/*.dylib; do
    [ -f "$lib" ] || continue
    minos="$(otool -l "$lib" 2>/dev/null | awk '/LC_BUILD_VERSION/{f=1} f && /^ *minos/{print $2; exit}')"
    if [ -z "$minos" ]; then
        echo "ERROR: $(basename "$lib") has no LC_BUILD_VERSION" >&2
        fail=1
    elif [ "$minos" != "$TARGET" ]; then
        echo "ERROR: $(basename "$lib") reports minos $minos, expected $TARGET" >&2
        fail=1
    fi
done
if [ "$fail" -ne 0 ]; then
    echo "The deployment target did not reach the linker — do not ship this." >&2
    exit 1
fi

rm -rf "$WORK_DIR"
echo "$STAMP_CONTENT" > "$STAMP"

echo
echo "macOS deps built for deployment target $TARGET:"
for lib in "$PREFIX"/lib/*.dylib; do
    [ -f "$lib" ] && echo "  $(basename "$lib")"
done
echo
echo "Point the build at them with:"
echo "  CMAKE_PREFIX_PATH=$PREFIX"
echo "  PKG_CONFIG_PATH=$PREFIX/lib/pkgconfig"
