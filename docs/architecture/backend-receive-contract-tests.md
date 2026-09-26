# Bounded receive-contract test foundation

Issue #5890 implements the first test slice of #5554 section 2.3, before the
first #5262 M4 receive-command conversion. This is not full backend
conformance certification and does not replace hardware convergence testing.

`tests/backend_receive_contract_test.cpp` keeps one test-only family registry
and one request group: frequency, mode, filter, and AGC mode/threshold. There
is no new production dispatch registry or protocol method. Each family prints
its coverage level, including an explicit `NOT BUILT` row for optional RTL.

| Family | Concrete implementation exercised | What this does not establish |
| --- | --- | --- |
| Flex | Exact guarded slice-sink commands; separately decoded status; no command echo from decode | Firmware acceptance, desktop command migration, pan creation through the compatibility adapter |
| Icom | Real CI-V scheduler with an unstarted transport; separately injected frequency/mode/filter/AGC reports and stale-generation rejection | Actual RS-BA1 delivery or firmware behavior |
| HL2 | Pre-connect receiver configuration, mode/filter preservation, AGC state, owner-thread signals | Live Metis delivery or completed DSP application |
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

The next M4 slice must extend this foundation with the actual changed dispatch
paths: explicit preserve-pan/recenter tuning intent, filter origin, and AGC
field selection; both desktop and daemon entry points; reconnect/reused slice
IDs; single dispatch; slice-link notification; and unchanged TX authority
guards. Host DSP/worker dispatch must be tested at an injected worker seam
before claiming coverage beyond the configuration-state rows above.

Both new tests are unconditional default CTests, registered in
`tests/tests.cmake`; they run in the main full-suite and sanitizer lanes.
The frozen per-PR CI allowlist is unchanged. Neither test opens a network
session or synthesizes third-party firmware, and neither invokes keying.
