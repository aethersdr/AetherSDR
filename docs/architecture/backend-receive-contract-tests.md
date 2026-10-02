# Bounded receive-contract test foundation

Issue #5890 seeds the first test slice of #5554 section 2.3. Issue #5904 extends
it through the first #5262 M4 receive-command conversion. This is not full backend
conformance certification and does not replace hardware convergence testing.

`tests/backend_receive_contract_test.cpp` keeps one test-only family registry
and one request group: frequency, mode, filter, and AGC mode/threshold. There
is no new production dispatch registry or protocol method. Each family prints
its coverage level, including an explicit `NOT BUILT` row for optional RTL.

| Family | Concrete implementation exercised | What this does not establish |
| --- | --- | --- |
| Flex | Exact guarded slice-sink commands, pan intent, filter origin, individual AGC fields; separately decoded status; no command echo from decode | Firmware acceptance, pan creation through the compatibility adapter |
| Icom | Real CI-V scheduler with an unstarted transport; separately injected frequency/mode/filter/AGC reports and stale-generation rejection | Actual RS-BA1 delivery or firmware behavior |
| HL2 | Pre-connect receiver configuration plus a socket-free real RX worker: mode/filter ordering, CW passband translation and applied AGC pair | Live Metis delivery, RF convergence or completed tuning/shift application |
| ANAN | Pre-connect receiver configuration and retained WDSP AGC values, owner-thread signals | Live Protocol 2 delivery or completed DSP application |
| Demo | Production synthetic session state and refusal before/after the session | Hardware behavior; filter/AGC DSP that Demo does not implement |
| RTL (optional) | Declaration and cold receive refusal, without opening USB | Connected USB/DDC dispatch |

Icom command checks inspect the first outbound entry in the production CI-V
trace, including its address and framing, rather than requiring an immediate
single dispatch. Confirmation reads and filter PBT writes may follow it. An
explicit scheduler-time advance exercises reply expiry and verifies that a
later dispatch cannot replace the first-command evidence; no sleeps or new
production test hooks are used. The two Icom-facing tests share one existing
friend-helper definition in `IcomReceiveContractTestAccess.h`.

The headless capability records are checked separately from desktop verbs.
Icom and ANAN mode/filter remain unadvertised to daemon clients; that does not
mean the desktop implementations are absent. Demo's stored filter state does
not justify advertising a functional daemon filter operation. No AGC protocol
method is added here.

`tests/icom_panadapter_capacity_test.cpp` covers every known Icom profile and
the unknown/no-scope fallback. It follows production backend setup and the
capability notification through `RadioModel` into `RadioResourceAdapter`.
The implemented scope owns one pan identity, so scope-capable dual-receiver
models advertise one panadapter, not two (#5347). Slice capacity is unchanged.
The desktop `maxPanadapters()` getter is checked for every scope-capable
profile. Its existing zero/unknown fallback is not redesigned here, nor is
the unsupported-versus-exhausted `panadapterLimitReached` signal distinction.
Optional filter-preset verbs are outside this first receive group.

Existing `control_receive_test` and `control_slice_frequency_test` retain the
direct daemon target/service tests. `backend_slice_lifecycle_test`,
`backend_family_switch_test`, and `backend_capability_revision_test` retain
the model lifecycle, compatibility-adapter, generation and revision checks.
An inherited lifecycle method returning false is not proof that Flex's or
hybrid Demo's model-owned compatibility path is broken.

`receive_intent_routing_test` drives both production model construction paths,
including repeat wiring, synchronous observations, reconnect/reused object IDs,
reentrant setters, off-thread refusal and frequency notification/provenance
ordering. The daemon tests pin explicit preserve-pan and operator-filter
requests while retaining admission checks and observation-only state. Actual
GUI linked-slice behavior still needs bridge proof; notification ordering alone
is not a substitute for the GUI's link adapter. The HL2 worker fixture uses the
existing friend access to open only its RX DSP and reads the applied channel on
the worker thread. It never starts Metis, connects hardware or configures TX.
ANAN remains configuration-state coverage, not a claim of DSP completion.

These tests are unconditional default CTests, registered in
`tests/tests.cmake`; they run in the main full-suite and sanitizer lanes.
The frozen per-PR CI allowlist is unchanged. None opens a network session,
synthesizes third-party firmware or invokes keying.
