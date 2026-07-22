// Hardware-free regression test for opt-in TX capture health summaries.
// It pins the strong PipeWire/Qt pull-mode signature without requiring an
// audio device: initial Idle is healthy; a full buffer is saturation even when
// PipeWire keeps reporting Active; Active -> Idle with unread bytes remains a
// fallback; later local TX attempts are counted and each anomaly class is
// reported only once per source lifecycle.

#include "core/TxCaptureHealthTracker.h"

#include <cstdio>

using AetherSDR::TxCaptureHealthTracker;

namespace {

int g_failed = 0;
int g_total = 0;

void expect(bool condition, const char* label)
{
    ++g_total;
    std::printf("%s %s\n", condition ? "[ OK ]" : "[FAIL]", label);
    if (!condition) {
        ++g_failed;
    }
}

} // namespace

int main()
{
    using CaptureState = TxCaptureHealthTracker::CaptureState;
    using Event = TxCaptureHealthTracker::Event;

    TxCaptureHealthTracker tracker;
    tracker.reset(CaptureState::Idle, 0);

    expect(tracker.observeState(CaptureState::Idle, true, 4096) == Event::None,
           "initial QAudioSource Idle is not a saturation event");

    expect(tracker.observeState(CaptureState::Active, false, 0) == Event::None,
           "Active marks the source as having delivered capture data");
    tracker.recordMicRead(25);

    expect(tracker.recordSuppressedCallback(4096, 8192) == Event::None,
           "partially filled Active capture remains healthy");
    expect(tracker.recordSuppressedCallback(8192, 8192)
               == Event::BufferSaturatedDuringTci,
           "full buffer reports saturation while capture remains Active");
    expect(tracker.recordSuppressedCallback(8192, 8192) == Event::None,
           "repeated full callbacks do not repeat the saturation record");

    expect(tracker.recordLocalTxAttempt(CaptureState::Active, true, false, true, 8192, 8192)
               == Event::None,
           "local TX while TCI audio is fresh is not classified as post-TCI");
    expect(tracker.recordLocalTxAttempt(CaptureState::Active, true, false, false, 8192, 8192)
               == Event::LocalTxWhileSaturated,
           "first post-TCI local TX against full Active capture emits an anomaly");
    expect(tracker.recordLocalTxAttempt(CaptureState::Active, true, false, false, 8192, 8192)
               == Event::None,
           "later stalled local TX attempts are counted without log spam");
    expect(tracker.recordLocalTxAttempt(CaptureState::Active, false, false, false, 8192, 8192)
               == Event::None,
           "another client's TX is not attributed to the local capture source");
    expect(tracker.recordLocalTxAttempt(CaptureState::Active, true, true, false, 8192, 8192)
               == Event::None,
           "local DAX TX is not classified as stalled microphone capture");

    const TxCaptureHealthTracker::Snapshot snapshot = tracker.snapshot(1000);
    expect(snapshot.sourceWasActive && snapshot.saturationObserved,
           "summary preserves the strong saturation signature");
    expect(snapshot.tciSuppressedCallbacks == 3
               && snapshot.suppressedBufferPeakBytes == 8192,
           "summary reports suppressed callback count and peak unread bytes");
    expect(snapshot.fullBufferDuringTciObservations == 2,
           "summary counts direct full-buffer observations during TCI");
    expect(snapshot.idleDuringTciTransitions == 0,
           "Active PipeWire saturation does not require an Idle transition");
    expect(snapshot.postTciLocalTxWhileSaturated == 2,
           "summary counts every post-TCI stalled local TX attempt");
    expect(snapshot.lastMicReadAgeMs == 975,
           "summary reports age of the last successful microphone read");

    tracker.recordMicRead(1100);
    expect(tracker.recordLocalTxAttempt(CaptureState::Active, true, false, false, 1024, 8192)
               == Event::None,
           "successful microphone consumption clears current saturation");
    const TxCaptureHealthTracker::Snapshot recovered = tracker.snapshot(1200);
    expect(recovered.saturationObserved
               && recovered.postTciLocalTxWhileSaturated == 2,
           "recovery preserves history without inflating stalled TX counts");

    expect(tracker.recordLocalTxAttempt(CaptureState::Active, true, false, false, 8192, 8192)
               == Event::None,
           "a later real saturation is counted but remains rate-limited");
    expect(tracker.snapshot(1300).postTciLocalTxWhileSaturated == 3,
           "post-recovery count advances only for a newly full buffer");

    TxCaptureHealthTracker idleFallback;
    idleFallback.reset(CaptureState::Active, 0);
    expect(idleFallback.recordSuppressedCallback(4096, 0) == Event::None,
           "unknown capacity does not invent a full-buffer event");
    expect(idleFallback.observeState(CaptureState::Idle, true, 4096)
               == Event::BufferSaturatedDuringTci,
           "Active-to-Idle with unread bytes remains a saturation fallback");

    std::printf("\n%d of %d TX capture health tests failed.\n", g_failed, g_total);
    return g_failed == 0 ? 0 : 1;
}
