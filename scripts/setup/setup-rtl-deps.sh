#!/bin/bash
# Pinned RTL-SDR Blog + libusb, with float FFTW on Linux. No device access.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$SCRIPT_DIR/_verify_sha256.sh"
PREFIX="${RTL_DEPS_PREFIX:-$(pwd)/third_party/rtl-deps}"
WORK="$(pwd)/.rtl-deps-build"
SOURCES="$PREFIX/share/aethersdr-rtl-sources"
mkdir -p "$WORK" "$SOURCES"
JOBS="${CMAKE_BUILD_PARALLEL_LEVEL:-4}"
field() { python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))[sys.argv[2]][sys.argv[3]])' "$SCRIPT_DIR/rtl-dependencies.json" "$1" "$2"; }
fetch() {
    local name="$1" archive
    archive="$(field "$name" archive)"
    if [ ! -f "$SOURCES/$archive" ]; then
        curl -fL --retry 3 --connect-timeout 30 "$(field "$name" url)" -o "$SOURCES/$archive"
    fi
    verify_sha256 "$SOURCES/$archive" "$(field "$name" sha256)"
    tar xf "$SOURCES/$archive" -C "$WORK"
}
fetch libusb
fetch rtl
USB="$WORK/$(field libusb directory)"
RTL="$WORK/$(field rtl directory)"
cmake_args=()
if [ "$(uname -s)" = Darwin ]; then
    : "${MACOS_DEPLOYMENT_TARGET:?Set the deployment target explicitly}"
    export MACOSX_DEPLOYMENT_TARGET="$MACOS_DEPLOYMENT_TARGET"
    cmake_args=(-DCMAKE_OSX_DEPLOYMENT_TARGET="$MACOS_DEPLOYMENT_TARGET" -DCMAKE_INSTALL_NAME_DIR="$PREFIX/lib")
fi
(cd "$USB" && ./configure --prefix="$PREFIX" --libdir="$PREFIX/lib" --enable-shared --disable-static --disable-examples-build --disable-tests-build)
make -C "$USB" -j"$JOBS"
make -C "$USB" install
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
USB_LIBRARY="$PREFIX/lib/libusb-1.0.so"
[ "$(uname -s)" != Darwin ] || USB_LIBRARY="$PREFIX/lib/libusb-1.0.dylib"
cmake -S "$RTL" -B "$WORK/rtl-build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_INSTALL_LIBDIR=lib \
    -DCMAKE_DISABLE_FIND_PACKAGE_Git=ON -DCMAKE_DISABLE_FIND_PACKAGE_PkgConfig=ON \
    -DLIBUSB_LIBRARIES="$USB_LIBRARY" -DLIBUSB_INCLUDE_DIRS="$PREFIX/include/libusb-1.0" \
    -DCMAKE_BUILD_WITH_INSTALL_NAME_DIR=ON -DINSTALL_UDEV_RULES=OFF -DDETACH_KERNEL_DRIVER=ON \
    ${cmake_args[@]+"${cmake_args[@]}"}
cmake --build "$WORK/rtl-build" -j"$JOBS"
cmake --install "$WORK/rtl-build"
# macOS setup already builds both FFTW precisions at the pinned floor.
if [ "$(uname -s)" = Linux ]; then
    fetch fftw
    mkdir -p "$WORK/fftw-build"
    (cd "$WORK/fftw-build" && "$WORK/$(field fftw directory)/configure" \
        --prefix="$PREFIX" --enable-float --enable-shared --disable-static --disable-fortran --disable-doc)
    make -C "$WORK/fftw-build" -j"$JOBS"
    make -C "$WORK/fftw-build" install
    cp "$WORK/$(field fftw directory)/COPYING" "$SOURCES/FFTW-COPYING"
fi
cp "$RTL/COPYING" "$SOURCES/RTL-SDR-COPYING"
cp "$USB/COPYING" "$SOURCES/libusb-COPYING"
cp "$SCRIPT_DIR/rtl-dependencies.json" "$SCRIPT_DIR/setup-rtl-deps.sh" "$SCRIPT_DIR/_verify_sha256.sh" "$SOURCES/"
echo "RTL dependency sources and recipes: $SOURCES"
