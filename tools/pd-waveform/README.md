# AetherPD native runtime

Experimental on-radio WDSP PureSignal runtime, integrated from the bench-tested
AetherPD baseline. It registers a PD waveform and responds to the radio's
transmit stream; it has no keying command. The prototype supports local IQ
from a separate capture helper, network IQ, and feedback audio.
The current modulator and demodulator implement USB only. USB/LSB support in
the receive-recovery journal describes cleanup profiles, not LSB RF support.

The FLEX-8600 baseline at the 10 W setting improved paired IMD3 from about
−27.8/−27.3 dBc to −44.3/−50.1 dBc. Those are finite bench results, not complete
qualification of this managed-startup revision or the FLEX-6000.

The independent development installer passed a fresh FLEX-8600 installation,
automatic reboot recovery with the same new local identity, and receive-only
DAX IQ capture with FDX off. Temporary installation authority and session capture
resources were removed; the surviving shim had no capabilities. See
[the first-install qualification](request/FIRST-INSTALL.md) for evidence and
remaining production/FLEX-6000 gates.

Build from the repository root with Zig installed:

```sh
python3 tools/pd-waveform/build.py --output build/aetherpd --arch both --host
```

`--regression-tests` also builds five standalone DSP tests for both selected
ARM targets and, with `--host`, the host. They cover dispatch/freshness fences,
warm-model lifetime, phase settling, calibration amplitude coverage and EQ
worker results. They are not installed in the waveform package. The Linux
CTest graph registers these tests against the current core, plus a calibration
dispatch test that checks retirement before output. Datagram I/O is injected;
the tests create no sockets, radio connection or RF commands. ARM builds alone
do not qualify a Cortex-A8 or Linux 2.6 radio.

`AETHER_PD_OWNER_FD` optionally names an inherited read-only lifetime pipe.
Managed startup requires an unprivileged process with no effective, permitted,
or inheritable capabilities. The owner pipe is never read: hangup ends packet
processing even when data remains queued. An independent API guardian must
retain the leased connection until the radio can safely release PD mode.
Without the owner descriptor, existing standalone startup is preserved.

A separate [unkey-only guardian candidate](./guardian/README.md) is under
development. It uses a prepared GUI-bound connection and cannot key the radio.
The shim builder packages it with `--with-aetherpd`, and managed startup
requires it before starting the DSP. A trusted provider must still prepare
the control connection and lifetime descriptors. No helper is installed by
building a package.

`AETHER_PD_STATUS_FD` optionally names a private connected Unix datagram
socket, paired with an owner descriptor. It replaces the TCP status listener.
Only a nine-byte status request (`?` and eight request-counter bytes) is
accepted; replies echo the counter followed by the bounded JSON snapshot.
It has no RF or lifecycle control verbs. The managed shim creates this pair
itself and keeps stop on a separate lifetime pipe.

The shim supplies this contract through its trusted launcher handoff. The
runtime, guardian, and capture helper are distinct responsibilities. No
privileged IQ provisioning service is installed by this builder.

The [fixed capture constructor](./capture/README.md) builds the packet socket
inside the same trusted executable as its strict ring worker. Its current
profile is FLEX-8600 only; ARMv7 compilation does not establish a FLEX-6000
network profile or a supported way to obtain setup privileges.

A bounded FLEX-8600 firmware4.3.16 bench run demonstrated local DAX IQ
feedback and AetherPD correction with the radio's FDX setting disabled. During
7.7 seconds of keying at a10W setting,2091 IQ packets were captured and all120
GUI observations reported FDX off. Correction became active at about6.0s and
remained active until operator unkey. One capture's0.3s before/after portions
changed IMD3 from approximately−27/−27dBc to−43/−54dBc; tone reference power
changed0.31dB. This establishes a short proof, not sustained qualification or
an independent matched OFF/ON comparison. It does not establish ordinary RX
audio behavior, FLEX-6000 support, or that every half-duplex implementation can
supply feedback. Actual IQ availability during TX is the requirement; the
FDX control alone does not establish that availability.

The runtime also has an optional private pre-API startup barrier for the
[prepared scheduling handoff](./SESSION-PROVIDER.md#prepared-dsp-scheduling-handoff).
Existing launches omit it. It is a constructor/guardian ordering mechanism,
not a user control or a complete privileged session provider. The fixed C
constructor now creates and schedules its own DSP child, retires privilege,
and executes Go as the same parent. Go validates the child and arms guardians
before release. ARM64 and ARMv7 receive-only tests passed on the8600; the
complete network/resource provider and supported privilege provisioning remain
unfinished. This handoff has separate RF and failure-lifecycle qualification
requirements.

A fresh nonlinear calibration after a transport stall can remain incomplete:
insufficient amplitude coverage keeps the runtime bypassed. It does not
inject diagnostic tones on recovery.
An 8600 warm-voice bench probe left about 15 seconds after a controlled
20 ms pause. Alignment and EQ returned, but both highest collection bins
remained empty and correction did not resume. This is an unresolved learning
coverage limitation, not evidence that a longer socket timeout fixes recovery.

The managed session constructors enable validated recent-model recovery.
A completed-send deadline gap quarantines a
same-configuration PA curve at most 30 seconds old. Output bypasses it while
alignment and EQ are rebuilt. Reuse requires fresh IQ, an equivalent EQ fit
(normalized prediction-shape error at most 5% on fresh excitation, after a
common complex gain; residual at most 1%), and unchanged
RF configuration. Send errors, stale IQ, configuration invalidation, native APD,
and unkey discard a pending candidate. This extends the clean-unkey cache
assumption; a matching linear fit cannot independently prove that every
unexcited nonlinear amplitude is unchanged. This is an operator-approved
production behavior, with further qualification still required. Standalone
bench launches retain an explicit `AETHER_PD_TRANSPORT_CACHE=1` opt-in.

Two bounded FLEX-8600 warm-voice trials recovered from a 20 ms process pause
in about 3.9 seconds. Packet traces showed continuous cached correction for
the remaining 11.3 seconds of each key. Matched post-recovery speech improved
adjacent-channel power by 12.8/13.2 dB and 11.8/14.5 dB; every fixed matched
interval, including the final partial interval, improved. These are finite
10 W-setting bench results for that recovery path, not general fault recovery
or production qualification. Other fault types still discard the model.

Build its standalone regression tests with `build.py --transport-tests` in
addition to the normal build arguments. The test executables are not added to
the waveform package.

A small standalone pipe test is available:

```sh
cc -std=c11 -O2 tools/pd-waveform/owner_test.c tools/pd-waveform/pd_owner.c -o /tmp/pd-owner-test
/tmp/pd-owner-test
```

WDSP and musl notices accompany the generated waveform package. Corresponding
sources are here and in `third_party/wdsp`; no FFTW library is linked.

Managed operator calibration uses a separate `AETHER_PD_CALIBRATION_FD`
private datagram pair. It accepts only nine-byte `A`(calibrate), `M`(measure)
or `X`(cancel)
requests followed by a nonzero monotonic BE64 counter; replies contain the
counter and a one-byte result. Status FD7 retains its read-only protocol.
The managed Go launcher creates the pair and forwards the prepared GUI handle
as fixed metadata. Arming requires that GUI's selected PD slice, idle state,
fresh local IQ and no native APD. Only the same GUI's software PTT starts the
fixed eight-second source; another owner, malformed ownership, configuration
changes or faults cancel it. The arm expires after thirty seconds without PTT.
A sample limit and per-packet monotonic deadline checks bound generation.
Neither arming nor cancellation keys the radio; cancellation removes the source.
Saved per-band transmit settings do not change the live power value or cancel
an arm. A change to live transmit power still cancels calibration.
Measurement uses the same bounded source without invalidating the learned PA
model. Source retirement logs a fixed reason, source identity, phase and stream
counters once; subsequent inactive packets do not repeat the record. These
diagnostics distinguish admission, configuration, continuity and deadline
cancellation without changing any source limit or automatically rearming it.

The receive observer first identifies the session's GUI from subscribed client
inventory, then binds its own API connection to that exact client ID. It requires
the bind reply to name the expected handle and a complete interlock observation
after the bind request. The unbound interlock is discarded: on FLEX-8600 firmware
4.3.16 it can report `RECEIVE` with TX denied while the GUI reports `READY`.
Binding does not key, select a receiver or restore settings. Snapshot freshness,
ownership and idle-state checks still apply.

Recovery is not complete: losing the leased radio API connection removes the
active PD slice on the tested FLEX-8600, even without PTT. The independent
unkey connection survives, but the lease guardian cannot restore USB through
the lost connection. Recreating the receive slice requires an explicit
client action. Process-death recovery with the API lease intact does not
qualify this separate failure path.

The native process exits with code 7 for a failed leased API connection, so the
owning shim can distinguish that event from a normal stop or generic process
failure. A session journal records the pre-PD receive snapshot and a fresh
owned-PD observation. The authenticated development receive-restore operation
can reapply modeled settings after a surviving receiver returns to its original
mode. For a measured basic single-receiver USB/LSB profile, it can also
recreate a receiver removed by confirmed API loss, after fresh ownership and
resource checks. It validates the create reply and fresh receiver identity
before restoring settings; an ambiguous create reply cannot be retried.
Advanced profiles are refused for allocation recovery. This path uses ordinary
radio API access and never keys or rearms the waveform.

`build.py --calibration-tests --host` also builds the independent timing/replay
and native admission/cancel tests. The socket-free dispatch test injects a
20 ms output gap through `on_packet`, verifies cancellation before generated
output, and checks that microphone packets continue without automatic rearm.
Authenticated control and the fixed calibration source have finite RF coverage
on the FLEX-8600 with firmware 4.3.16.42034. Two independent 17-second OFF/ON
tone pairs at the 10 W setting retained about 17–21 dB of IMD3 improvement
after the source returned to PC audio. Their tone levels matched within
0.32 dB, with no IQ sequence gaps, input limiting or reported transport faults.
Three 17-second keys in one managed session also retained correction with a
single initial arm and experimental transport recovery disabled. The test
wrapper timed out reading its temporary status mirror during final teardown;
the original failure is retained alongside the successful RF captures,
independent unkey records and exact native/radio cleanup. These finite results
do not qualify long continuous operation or an ordinary-user installer.

A separate bounded comparison used the radio's stock APD with its ANT1 sampler
set to RXA, never simultaneously with AetherPD. Corrected tone output matched
within 0.1 dB. In the final two-second window, stock APD measured
−48.1/−46.5 dBc IMD3 and AetherPD −47.4/−49.2 dBc, referenced to each tone.
This single pair per implementation does not establish a general winner,
speech equivalence or cold training time. Unprocessed speech qualification
remains unresolved: its upper adjacent-band measurement was close to the
same capture's noise floor and did not consistently improve.

Two independent pairs with offline crest-reduced, band-limited speech passed
the matched-interval check. The second pair matched occupied output within
0.064 dB and improved adjacent-channel power by 15.2–17.7 dB across its three
late two-second captures; both sides improved in every fixed half-second
interval and final partial interval. No input limiting, sequence gaps or
reported transport faults occurred. The processing was applied to the test
file, with AetherTX bypassed; this is coverage of that processed excitation,
not a claim about every live microphone or processing configuration.

The [FLEX-6000 handoff](./FLEX6000-HANDOFF.md) lists the hardware, Linux 2.6,
storage and narrow-provider gates that compilation and 8600 RF tests cannot
establish. The released Tailscale service does not include these development
calibration controls.

The standalone Linux build enables priority inheritance for WDSP critical
sections with `AETHER_PD_PI_CRITICAL_SECTIONS`. Its RR packet thread shares
these locks with ordinary fit workers. Bounded synthetic contention on the8600
made the former plain lock delay packet correction by up to21.2ms onARM64 and
23.0ms onARMv7; the same probes with donation stayed below1.3ms. These are
injected CPU contention measurements, not the cause of the historical natural
slip or an RF qualification of this build. Lock initialization/operation errors
terminate the process rather than continue without a valid lock; the independent
guardians remain responsible for unkey and receive recovery. Desktop WDSP does
not enable this option. Linux2.6 kernel PI support still requires actual6300
qualification, including a contended mutex test.

### Alignment failure evidence

When aligned or cached correction loses TX continuity, misses a dispatch
boundary, fails a send, loses feedback freshness/continuity, or loses
coherence, stderr records `alignment evidence`. The bounded snapshot captures
packet counters, generation, FIFO depths, delay/coherence and timing before
reset; output occurs after correction retirement. It opens no diagnostic
file and adds no per-packet logging. `session_max_*` values include earlier
keys and must not be treated as the latency of that event. TX and IQ guard
counters have their own generation/reset lifetimes.

These records distinguish observed transport failures from a coherence-only
failure. They do not prove which FPGA behavior caused a delay change, nor
attribute a historical event whose packet trace was not captured.

Consumed delay estimates also produce `alignment evidence: reason=acquisition` records in the
same bounded retained diagnostic slot. Source/generation and radio monotonic
timestamps distinguish collection before submission, worker queueing,
estimation, and consumption by the packet thread. Rejected estimates remain
visible. These observations do not change alignment acceptance or RF duration;
the latest event can overwrite a reset record, and the count covers both kinds.
