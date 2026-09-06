# tests/

Automated unit tests — `*_test.cpp` files compiled by CMake and run
in CI, except the retired fixtures listed under "Network-fixture
boundary" below, which stay in source for history but are not
configured or compiled. To run the suite locally:

```sh
cmake -B build -S .
cmake --build build --target test
ctest --test-dir build --output-on-failure
```

## Adding a test

Drop `<feature>_test.cpp` into this directory, then declare its
`add_executable` + `add_test` in **[`tests/tests.cmake`](tests.cmake)** — *not*
in the top-level `CMakeLists.txt`, where these declarations used to live. There
is **no glob**: every test is declared explicitly, so copy the block of a
neighbouring target.

Write source paths **relative to the repository root**, exactly as you would
have in the root file — `tests/my_new_test.cpp` and `src/gui/Bar.cpp`, not
`my_new_test.cpp` and `../src/gui/Bar.cpp`. `tests.cmake` is pulled in with
`include()` rather than `add_subdirectory()` precisely so that stays true; the
header of that file explains why, and why it should not be "tidied up" into a
subdirectory later.

A test that touches `AppSettings` compiles `${AETHER_SETTINGS_SOURCES}` and
needs `aether_sqlite3` — add its target name to the `AETHER_SETTINGS_CONSUMERS`
list at the bottom of `tests.cmake`.

Putting a test target in the root `CMakeLists.txt` instead fails two ways, on
purpose: `tests.cmake` aborts the CMake configure step with a message pointing
here, and `tools/check_test_registration.py --strict` fails the PR in CI.

## Network-fixture boundary

Backend and automation behavior that depends on a local synthetic network peer
is retired from the default graph along three lines: deterministic protocol,
codec, model, and policy assertions stay socket-free in this suite; dropped
packets, refusals, disconnect edges, and TX interlocks are expressed as
injected transport/state-machine tests rather than socket fixtures; and
positive convergence against real firmware is proven through the automation
bridge and `radiocert`, which certify by effect and cannot prove a non-event.

The following 12 positive-convergence fixtures remain in source and as bracket
comments in `tests.cmake` for history, but are not configured, compiled, or
registered with CTest:

- Icom: `icom_session_test` and `icom_backend_test`.
- HL2: `hl2_signal_stop_test` and its helper, `hl2_metis_client_test`,
  `hl2_backend_test`, `hl2_link_stats_test`, and
  `hl2_link_stats_model_test`.
- Automation server: `automation_json_id_test`,
  `automation_connect_wait_phase_test`, `automation_double_click_test`,
  `automation_fm_repeater_verbs_test`, and `automation_drag_at_test`.

Three socket fixtures remain registered until their negative assertions have
socket-free replacements: `vkamp_connection_test` (bypass/antenna interlocks),
`automation_server_gesture_test` (TX-keying refusals and cleanup), and
`hl2_receiver_count_restart_test` (dropped Metis-start retry). The IC-9700
capability-table assertion that used to carry this lived in
`radio_capability_gating_test`, which was removed for intermittency — so
RadioModel's application of that band ceiling to the transmit model now has no
registered test at all, alongside its socket-free replacement in #5254.

Two HL2 tests are explicit rather than part of the default graph:

- Both weekly sanitizer lanes enable `hl2_receiver_churn_test` with
  `-DAETHER_ENABLE_HL2_RECEIVER_CHURN_TEST=ON` — TSan for the receiver-vector
  race, ASan for the use-after-free class — while a socket-free concurrency
  harness is designed.
- An operator running `./hpsdrsim -hermeslite2 -P1` may enable and run
  `hl2_tx_loopback_test` with `-DAETHER_ENABLE_HL2_TX_LOOPBACK_TEST=ON`.
  The test fingerprints the simulator before it can key. The weekly sanitizer
  lanes build it for compile coverage; without a simulator it skips honestly
  (exit 77, reported by ctest as Skipped).

**Not to be confused with [`/docs/qa/`](../docs/qa/)**, which holds
*manual* QA checklists and test plans — human procedures for features
that need a real radio to exercise. Different artifact, different
audience: that directory is for procedures; this one is for code.
