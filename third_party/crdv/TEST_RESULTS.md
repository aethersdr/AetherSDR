<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Verification results

- Date: 2026-07-11
- Host: macOS, AppleClang 21.0.0.21000101, CMake 4.3.3
- Language mode: C17 with extensions disabled

## Warning-clean build

Configured with the project warning set plus `-Werror`:

```text
cmake -S . -B .build-macos-warning \
  -DCMAKE_BUILD_TYPE=Debug -DCRDV_BUILD_TESTS=ON -DCMAKE_C_FLAGS=-Werror
cmake --build .build-macos-warning --parallel
ctest --test-dir .build-macos-warning --output-on-failure
.build-macos-warning/crdv_tests
```

Result: build succeeded without warnings; CTest reported 100% passed and 0 failed tests; `crdv_quarantined` skipped by its declared return code 77. Direct deterministic result: `229652 checks, 0 failures`.

## ASan and UBSan

```text
cmake -S . -B .build-macos-sanitize \
  -DCMAKE_BUILD_TYPE=Debug -DCRDV_BUILD_TESTS=ON \
  -DCRDV_ENABLE_SANITIZERS=ON -DCMAKE_C_FLAGS=-Werror
cmake --build .build-macos-sanitize --parallel
ctest --test-dir .build-macos-sanitize --output-on-failure
.build-macos-sanitize/crdv_tests
```

Result: build succeeded; `crdv_tests` passed with `229652 checks, 0 failures`; no AddressSanitizer or UndefinedBehaviorSanitizer report; quarantined hardware acceptance skipped; 0 failed tests.

## Static analysis

```text
clang --analyze -std=c17 -Iinclude \
  -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror \
  -Xanalyzer -analyzer-output=text \
  src/air.c src/config.c src/control.c src/dv3000.c src/modem.c src/vita.c
```

Result: AppleClang static analysis completed over all six library translation units with no diagnostics. The compiler warning set was separately promoted to errors for the library and tests. A whitespace scan over source, tests, CMake, and Markdown found no tabs or trailing blanks.

## Linux build

The deliverable was mounted read-only at `/src` in an ephemeral arm64 `debian:trixie-slim` container. GCC 14.2.0 and CMake 3.31.6 built in `/build` with C17, the project warning set, and `-Werror`:

```text
cmake -S /src -B /build -DCMAKE_BUILD_TYPE=Debug \
  -DCRDV_BUILD_TESTS=ON -DCMAKE_C_FLAGS=-Werror
cmake --build /build --parallel
ctest --test-dir /build --output-on-failure
/build/crdv_tests
```

Result: build succeeded; CTest reported 100% passed, 0 failed tests, and the hardware acceptance skip; direct deterministic result was `229625 checks, 0 failures`. No AetherSDR image, checkout, object, or artifact was mounted or inspected. This Linux result predates the post-delivery preamble-qualification correction and is retained as historical platform evidence rather than a claim about that correction.

## Short-message mini-header correction coverage

The deterministic `test_short_message_miniheaders` regression verifies that one-based API blocks 1-4 transmit wire mini-headers `0x40`, `0x41`, `0x42`, and `0x43`; all four exact scrambled V2 vectors match; RX assembles those four blocks out of order into the original 20-byte message; and wire mini-header `0x44` returns `CRDV_E_FORMAT` without publishing. C5 is resolved by JARL D-STAR Standard 7.0 section 6.3 and no longer appears in `crdv_quarantined`.

## Receive synchronization policy coverage

The deterministic suite verifies:

- the all-zero policy remains exact-only, fixed-boundary, non-unlocking, and non-sliding;
- every maximum Hamming-distance boundary from 0 through 24;
- all 24 one-bit locations, all 276 two-bit combinations, and all 2,024 three-bit combinations for exhaustive exact/one-bit/two-bit policy acceptance and immediate false-candidate boundaries;
- every early and late realignment offset from 1 through 24 bits, including restoration of the following 96-bit frame boundary and suppression of the ambiguous current AMBE frame;
- tolerant off-boundary sync, rejected near-candidates, late search look-ahead preservation, expected-miss accounting, and lock loss on the configured consecutive limit; and
- exactly one event for each accepted sync/miss/loss and exactly one end callback for normal tail or policy lock loss.

These are mechanism tests only. No Hamming threshold, miss limit, sliding enablement, or window span is asserted as AetherSDR-compatible; C11 remains skipped pending independent BER/noise/hardware evidence.

## Post-delivery preamble acquisition correction

The current macOS warning-as-error and ASan/UBSan configurations were rerun
after adding preamble-qualified initial acquisition. Both CTest runs passed;
`crdv_tests` reported `229652 checks, 0 failures`, and the quarantined hardware
acceptance test remained skipped. The added cases prove that zero, one, and two
errors in the final 16 alternating qualifier bits admit the unchanged exact
frame sync, while three errors do not publish a header.

## Platform scope

The source and CMake avoid POSIX APIs and include an MSVC branch. macOS/AppleClang and Linux/GCC passed. A Windows executor or cross-toolchain was not installed, so Windows remains compile-structure reviewed rather than claimed as run. Real ThumbDV, FLEX-6000, RF analyzer, independent D-STAR receiver, and timing acceptance remain explicitly quarantined in C1-C4 and C6-C17.
