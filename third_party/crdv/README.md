<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# CRDV clean-room D-STAR / ThumbDV library

CRDV is an independently authored, transport-neutral C17 library for the non-AMBE portions of a host-attached ThumbDV D-STAR service. It implements configuration validation, D-STAR header protection and recovery, slow data, short messages, header repetition, hard-bit receive framing, opt-in tolerant data-sync tracking and bounded sliding realignment, caller-parameterized Gaussian discriminator shaping, DV3000 envelopes and transaction generations, FlexRadio audio VITA packets, bounded line parsing, metrics validation, and correlated SmartSDR responses. Short-message TX and RX use the JARL 7.0 section 6.3 wire mini-headers `0x40` through `0x43`; `0x44` is not accepted as a short-message block.

It does not contain an AMBE codec, serial-port implementation, socket implementation, radio-keying code, executable supervisor, or GPLv3 SmartSDR transport. A parent application supplies those pieces through the interfaces in `include/crdv.h` and the rules in `INTEGRATION_CONTRACT.md`.

## Build and test

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

For a Clang/GCC sanitizer pass:

```sh
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DCRDV_ENABLE_SANITIZERS=ON
cmake --build build-asan --parallel
ctest --test-dir build-asan --output-on-failure
```

The library uses only the C17 standard library. CMake selects `m` for non-MSVC toolchains and avoids POSIX-only APIs. Windows callers may use `COMn` or `\\.\COMn`, but serial opening and exclusive ownership intentionally remain in the parent transport.

## Source layout

- `include/crdv.h`: sole public API.
- `src/air.c`: PFCS, header FEC/interleave/scramble, framing, messages, header repetition, and caller-configured receive-sync policy.
- `src/modem.c`: parameterized Gaussian pulse construction and discriminator sample conversion.
- `src/dv3000.c`: bounded DV3000 packets, documented startup requests, PCM/channel conversion, and generation-aware transactions.
- `src/vita.c`: exact 24 ksample/s stereo-float Flex VITA audio packet validation and serialization.
- `src/control.c`: bounded line records, responses, stream IDs, metrics, and command correlation.
- `src/config.c`: uppercase/padding/default and boundary policy.
- `tests/test_main.c`: deterministic vectors, mutations, fragmentation, rollover, malformed input, and seeded hostile-input checks.
- `tests/quarantined_acceptance.c`: unresolved C1-C4 and C6-C17 hardware evidence gates; CTest reports this test as skipped.

## Deliberate evidence gates

No default BT/span, Flex polarity inversion, silence AMBE word, hardware deadline, queue capacity, drain bound, receive sync tolerance/miss limit/sliding behavior, fallback baud rate, USB VID/PID, fixed UDP port, or RF keying policy is embedded. Receive tolerance is opt-in through `crdv_receive_sync_policy`; its all-zero value retains exact fixed-boundary recognition and never drops lock for missed sync. Compatibility values remain integration decisions until the evidence listed in `INTEGRATION_CONTRACT.md` exists. C5 is resolved by the public JARL standard and is no longer an evidence gate. The library will not key a radio.

## License

SPDX: `GPL-3.0-or-later`. The full GPLv3 text is in `LICENSE`; the “or later” choice is stated by every source-file SPDX identifier.
