# Overlay triplet: standard x64-windows (dynamic CRT, dynamic libs) but
# RELEASE-ONLY. AetherSDR's CI/release builds are Release, so building vcpkg
# deps (grpc + abseil/protobuf/openssl/re2/c-ares) in debug too just doubles
# build time and disk — which overflowed the runner's C: drive when grpc was
# added. Release-only halves both.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)
set(VCPKG_BUILD_TYPE release)
