# RTL M1 capture and audio runtime

M1 connects the transactional capture owner, prepared receiver registry,
per-receiver RF extraction, typed PCM and accepted-state settings. It remains
receive-only. The qualified production admission constant is **one**; eight
stable slots are storage, and four receivers are an offline measurement target.
Increasing admission requires the integrated architecture evidence below.

## Acquisition and ownership

A complete desired state includes center, achieved sample rate, direct sampling,
offset tuning, PPM, tuner gain and all receiver passbands. Hardware changes
quiesce USB, apply and verify the complete readback, prepare compatible DSP and
adopt it before acknowledgment. Preparation failure after a hardware change
compensates hardware and prepares the restored state. Failed compensation
withdraws valid capture. Superseded results cannot publish or persist.

Receiver-only changes prepare on the existing bounded registry pool while USB
continues. The acquisition context adopts the requested revision at a sample
boundary. Unchanged FM receivers share ownership across banks; callbacks only
borrow pointers. Final destruction and WDSP planning run on the pool. A reused
slot waits for retirement and gets a new instance. The mailbox holds one work
item; the transaction owner coalesces one further complete desired state.

## FM extraction and mixing

FM/FMN use separate phase-continuous NCOs, paired r8brain histories and fixed
1024-sample planar WDSP blocks at 48 kHz. Full filter edges plus a 3 kHz guard
must fit the capture. A gap, invalid block or WDSP underrun withdraws the
receiver; replacement is prepared away from acquisition. WDSP's nonblocking
exchange advances its ring on underrun, so continuing that instance could
mislabel stale samples. Unchanged siblings retain history and keep progressing.

A receiver joining an existing capture starts at the next exact coincidence of
its integral hardware sample clock and the 48 kHz clock. This is at most one
second for a coprime rate (usually far shorter). NCO phase uses the absolute
capture position. Arbitrary input partitioning cannot change converter calls.
All FM receivers use the same fixed-rate graph/filter length and retain its
common causal delay; output positions label that common output timeline.
Generated late-join comparisons check alignment after startup transients.

Each independent slice tap precedes monitor gain, mute and balance. The mixer
retains independent left/right channels and indexes fixed queues by sample
position. It emits 128-frame stereo quanta. A missing slice has a 2048-frame
(42.67 ms) deadline; expired positions become silence and late audio is dropped,
never moved to the current clock. Each slot has 8192 frames of storage. Summed
monitor levels use 1/N headroom followed by bounded clipping; adaptive level
normalization is outside M1. A 128-packet SPSC mailbox carries owned numeric
blocks to the backend thread, which constructs revocable typed PCM. Queue gaps
are marked discontinuous. `AudioEngine::DEFAULT_SAMPLE_RATE` remains 24000 and
the audio device's output rate remains independently negotiated.

WFM and the other existing non-FM modes retain their exclusive legacy DDC and
24 kHz format. Their demodulation is unchanged; monitor mute continues to clock
an independent pre-monitor tap. They cannot join a multi-FM bank. Selecting FM
with an incompatible inherited wide/sideband filter chooses a 16 kHz passband;
ordinary filter requests and saved restores are never resized. This mode
transition needs maintainer UX review. WFM stereo and normalization qualification
remain S1/S2; no replacement WFM claim is made here.

## Accepted-state persistence

RadioModel supplies its settings scope and reported-serial identity through a
neutral seam hook. The backend does not derive persistence identity from a
later USB enumeration. After accepted readback, successful migration/claim of
`RtlSlices` removes Tuning, Passband and SpanRate from generic OperatingState
ownership together. RF gain uses `storeRtlRfGainPreservingLegacy`, retaining the
legacy downgrade snapshot and unknown fields. A handled but refused feature
write never falls through to a second generic writer.

Saved receivers are considered in ascending stable-ID order against the
accepted, fixed capture. Restore does not retune, resize a passband or move a
sibling. Omitted entries survive reduced admission and out-of-window restore;
explicit accepted removal is separate. No fitting entry retains the valid
initial receiver. Pending or refused requests never feed the document writer.
Monitor controls are prepared with the bank and applied before its first block.
Existing stored AGC/squelch fields remain preserved; M1 does not claim new controls for those legacy
capability gaps. Real numeric USB serials remain identities; synthetic indices
use the model's anonymous family scope. Duplicate real serials still share one
settings identity and cannot be distinguished by this schema.

## Evidence and admission gates

Socket-free tests cover transactional failure/compensation, delayed retirement,
registry reuse, four distinct generated FM carriers through extraction and real
WDSP, arbitrary/one-sample chunks, capture edges, late-join alignment, mixer
holes/headroom/stereo, native PCM revocation and slot reuse, and persistence
ownership. The ordinary-C++ allocation probe does not intercept malloc, Qt or
private allocators. WDSP's C allocation guard now observes the executing thread;
its process-wide counter remains available for resource accounting. RTL warms
platform TLS before entering acquisition callbacks.

Before raising `kQualifiedReceiverCapacity`, freeze 1/2/4-distinct-FM workloads
and run at least 30 minutes with the actual integrated application, spectrum,
USB and audio paths on the target architecture. Require zero unexpected USB
restarts, zero stale-epoch acceptance, zero steady-state queue drops or receiver
withdrawals, bounded late-slice silence, and continuous sample counts. Record
CPU, thermal state, callback latency, startup/planning latency and cancellation
latency; admission needs headroom rather than component-only throughput. Cold
and warm FFTW wisdom runs use separate task-owned directories and production
planning. Native Mac/Linux/Windows RTL-on/off builds, ASan/UBSan, instrumented-Qt
TSan, affected A-series tests and real receive convergence are distinct evidence.
Linux ARM needs representative native hardware; macOS ARM is not a substitute.

The shared model's accepted-intent routing also depends on PR #5919. This
implementation adds only the neutral persistence handoff there; it does not copy
that PR's pending routing changes. Refresh and integrate the dependency before
claiming end-to-end model acceptance or release readiness.


### Runtime diagnostic readback

The existing backend `health` snapshot exposes cumulative per-connection
`rtlQueueDrops`, `rtlMixerLateFrames`, `rtlMixerRejectedBlocks` and
`rtlMixerConfigurationFailures`. The acquisition callback publishes only atomic
counter mirrors; the backend's existing service timer samples them into its
owner-thread cache. Counters are independently sampled, not a coherent event
trace. Legacy-only sessions leave the FM pipeline values unreported until that
pipeline has actually processed a callback; disconnected snapshots are empty.
A mixer configuration rejection discards its old buffered audio and requests
repair through the existing transaction owner before any receiver processing.

These counters do not measure callback p99/max, USB control latency, RF extractor
group delay, or the frozen 1/2/4-receiver hardware workload. Those qualification
gates remain open. The shared-model dependency also needs an explicit deferred
acceptance contract: #5919's typed dispatch is not acknowledgment, and its RTL
audio adapter must preserve sparse stable receiver IDs during integration.
