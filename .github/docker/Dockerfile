FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

# Build dependencies for AetherSDR CI
# Must include everything CMakeLists.txt can find_package() for
RUN apt-get update && apt-get install -y \
    cmake \
    ninja-build \
    pkg-config \
    g++ \
    git \
    patch \
    curl \
    make \
    python3 \
    qt6-base-dev \
    qt6-multimedia-dev \
    qt6-websockets-dev \
    qt6-serialport-dev \
    qt6-base-private-dev \
    qt6-shader-baker \
    qt6-shadertools-dev \
    libgl1-mesa-dev \
    libasound2-dev \
    libfftw3-dev \
    libhidapi-dev \
    qtkeychain-qt6-dev \
    portaudio19-dev \
    libpipewire-0.3-dev \
    autoconf \
    automake \
    libtool \
    libssl-dev \
    libmosquitto-dev \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*
