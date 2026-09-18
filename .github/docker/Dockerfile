FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

# Links this package to its repository on GHCR. It is what attaches the
# package to aethersdr/AetherSDR — the source link on the package page, and
# the repo-scoped access a private package would inherit. The package is
# public today, so nothing depends on the access half; the attachment is
# worth keeping regardless.
LABEL org.opencontainers.image.source=https://github.com/aethersdr/AetherSDR

# Qt comes from aqtinstall, NOT from the distro — see the Qt layer below.
# Ubuntu 24.04 ships Qt 6.4.2, which is seven minor versions behind current and
# has been EOL upstream since the 6.4 series ended at 6.4.3. It is also BELOW
# what AetherSDR ships: release binaries are built against Qt 6.8.3 LTS
# (.github/workflows/appimage.yml, and check-windows pins the same 6.8.3). A CI
# image on 6.4.2 therefore tested a Qt nobody runs, and rejected code that is
# valid on every shipped configuration — PR #4646 hit exactly that with
# QList::assign(), a Qt 6.6 addition.
#
# The base image still supplies gcc and glibc; only Qt is taken from aqt.
RUN apt-get update && apt-get install -y \
    cmake \
    ninja-build \
    pkg-config \
    g++ \
    ccache \
    git \
    patch \
    curl \
    make \
    python3 \
    python3-venv \
    libgl1-mesa-dev \
    libasound2-dev \
    libfftw3-dev \
    librtlsdr-dev \
    libhidapi-dev \
    portaudio19-dev \
    libpipewire-0.3-dev \
    autoconf \
    automake \
    libtool \
    libssl-dev \
    libmosquitto-dev \
    ca-certificates \
    libdbus-1-dev \
    libglib2.0-dev \
    libfontconfig1-dev \
    libfreetype6-dev \
    # libQt6Multimedia.so from aqt is linked against PulseAudio; without this,
    # every target using Qt6::Multimedia fails at link with undefined
    # pa_*@PULSE_0 references. The distro qt6-multimedia-dev pulled it in.
    libpulse-dev \
    libx11-dev \
    libx11-xcb-dev \
    libxcb1-dev \
    libxcb-cursor0 \
    libxcb-glx0 \
    libxcb-icccm4 \
    libxcb-image0 \
    libxcb-keysyms1 \
    libxcb-randr0 \
    libxcb-render-util0 \
    libxcb-shape0 \
    libxcb-sync1 \
    libxcb-util1 \
    libxcb-xfixes0 \
    libxcb-xinerama0 \
    libxcb-xkb1 \
    libxkbcommon-dev \
    libxkbcommon-x11-dev \
    && rm -rf /var/lib/apt/lists/*

# ── Qt 6.8.3 LTS ─────────────────────────────────────────────────────────
# Version and module list deliberately match appimage.yml and the Windows leg
# in ci.yml, so every artifact and every CI check compiles against one Qt.
# Bumping it means bumping all three together.
#
# The xcb/xkb/fontconfig -dev packages above are named explicitly because the
# distro qt6-*-dev packages used to pull them in transitively; without them,
# linking Qt6::Gui/Widgets fails on missing sonames.
#
# Ubuntu 24.04's pip is PEP 668 externally-managed, so aqt goes in a venv
# rather than --break-system-packages; the venv is removed in the same layer.
ENV QT_VERSION=6.8.3
ENV QT_ROOT=/opt/Qt/6.8.3/gcc_64
# aqtinstall is pinned. It is the tool that decides what Qt lands in this image,
# so leaving it floating means a cache-miss rebuild months from now silently
# fetches a different installer than the one this image was validated with.
ENV AQTINSTALL_VERSION=3.3.0
RUN python3 -m venv /tmp/aqt-venv \
    && /tmp/aqt-venv/bin/pip install --no-cache-dir "aqtinstall==${AQTINSTALL_VERSION}" \
    && /tmp/aqt-venv/bin/aqt install-qt linux desktop "${QT_VERSION}" linux_gcc_64 \
        -m qtmultimedia qtwebsockets qtserialport qtshadertools \
        --outputdir /opt/Qt \
    && test -x "${QT_ROOT}/bin/qmake" \
    && "${QT_ROOT}/bin/qmake" -query QT_VERSION \
    # qmake alone only proves qtbase landed. Assert each addon module's CMake
    # package too: a partial aqt install that still exits 0 would otherwise
    # surface much later as a confusing find_package(Qt6 COMPONENTS Multimedia)
    # miss during ci.yml's configure, with nothing pointing back here.
    && for m in Multimedia WebSockets SerialPort ShaderTools; do \
           test -f "${QT_ROOT}/lib/cmake/Qt6${m}/Qt6${m}Config.cmake" \
           || { echo "aqt did not install Qt6${m}"; exit 1; }; \
       done \
    && rm -rf /tmp/aqt-venv

# ── qtkeychain, built against THAT Qt ────────────────────────────────────
# Not the distro's qtkeychain-qt6-dev: that is compiled against Ubuntu's Qt
# 6.4.2, and mixing it with an aqt 6.8.3 build is the ABI mismatch
# scripts/setup/setup-qtkeychain.sh exists to avoid (see its header, and
# #3639 for what a missing keychain costs — SmartLink credential persistence
# silently compiles out via find_package(Qt6Keychain QUIET)).
#
# LIBSECRET_SUPPORT=OFF selects the pure Qt-D-Bus Secret Service backend for the
# same reason it does there.
#
# The version, commit pin and flags below are hand-duplicated from
# scripts/setup/setup-qtkeychain.sh rather than sourced from it: this image is
# built with `context: .github/docker` (see docker-ci-image.yml), so COPY cannot
# reach a file outside that directory. BUMPING QTKEYCHAIN MEANS EDITING BOTH.
ENV QTKEYCHAIN_VERSION=0.16.0
ENV QTKEYCHAIN_COMMIT=aa6da344e1a20b9194e12bace3665caeea6b6304
ENV QTKEYCHAIN_ROOT=/opt/qtkeychain
RUN git clone --depth 1 --branch "${QTKEYCHAIN_VERSION}" \
        https://github.com/frankosterfeld/qtkeychain.git /tmp/qtkeychain-src \
    && test "$(git -C /tmp/qtkeychain-src rev-parse HEAD)" = "${QTKEYCHAIN_COMMIT}" \
    && cmake -B /tmp/qtkeychain-build -S /tmp/qtkeychain-src -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_WITH_QT6=ON \
        -DBUILD_SHARED_LIBS=ON \
        -DBUILD_TRANSLATIONS=OFF \
        -DLIBSECRET_SUPPORT=OFF \
        -DCMAKE_PREFIX_PATH="${QT_ROOT}" \
        -DCMAKE_INSTALL_PREFIX="${QTKEYCHAIN_ROOT}" \
        -DCMAKE_INSTALL_LIBDIR=lib \
    && cmake --build /tmp/qtkeychain-build -j"$(nproc)" \
    && cmake --install /tmp/qtkeychain-build \
    && test -f "${QTKEYCHAIN_ROOT}/lib/cmake/Qt6Keychain/Qt6KeychainConfig.cmake" \
    && rm -rf /tmp/qtkeychain-src /tmp/qtkeychain-build

# CMake reads CMAKE_PREFIX_PATH from the environment, so ci.yml / codeql.yml /
# sanitizers.yml need no Qt-specific configure flags — their plain
# `cmake -B build` finds this Qt and this qtkeychain. LD_LIBRARY_PATH is what
# lets the offscreen ctest targets actually run.
ENV CMAKE_PREFIX_PATH=${QT_ROOT}:${QTKEYCHAIN_ROOT}
ENV Qt6_DIR=${QT_ROOT}/lib/cmake/Qt6
ENV PATH=${QT_ROOT}/bin:${PATH}
ENV LD_LIBRARY_PATH=${QT_ROOT}/lib:${QTKEYCHAIN_ROOT}/lib
