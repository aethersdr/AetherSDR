// Hardware-free regression test for default-on TX capture health summaries.
// It pins the strong PipeWire/Qt pull-mode signature without requiring an
// audio device: initial Idle is healthy; Active -> Idle after suppressed TCI
// callbacks with unread bytes is saturation; later local TX attempts are
// counted and each anomaly class is reported only once per source lifecycle.

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

    tracker.recordSuppressedCallback(4096);
    tracker.recordSuppressedCallback(8192);
    expect(tracker.observeState(CaptureState::Idle, true, 8192)
               == Event::BufferSaturatedDuringTci,
           "Active to Idle with suppressed callbacks and unread bytes reports saturation");
    expect(tracker.observeState(CaptureState::Idle, true, 8192) == Event::None,
           "unchanged Idle state does not repeat the saturation record");

    expect(tracker.recordLocalTxAttempt(CaptureState::Idle, true, false, true, 8192)
               == Event::None,
           "local TX while TCI audio is fresh is not classified as post-TCI");
    expect(tracker.recordLocalTxAttempt(CaptureState::Idle, true, false, false, 8192)
               == Event::LocalTxWhileSaturated,
           "first post-TCI local TX against saturated capture emits an anomaly");
    expect(tracker.recordLocalTxAttempt(CaptureState::Idle, true, false, false, 8192)
               == Event::None,
           "later stalled local TX attempts are counted without log spam");
    expect(tracker.recordLocalTxAttempt(CaptureState::Idle, false, false, false, 8192)
               == Event::None,
           "another client's TX is not attributed to the local capture source");
    expect(tracker.recordLocalTxAttempt(CaptureState::Idle, true, true, false, 8192)
               == Event::None,
           "local DAX TX is not classified as stalled microphone capture");

    const TxCaptureHealthTracker::Snapshot snapshot = tracker.snapshot(1000);
    expect(snapshot.sourceWasActive && snapshot.saturationObserved,
           "summary preserves the strong saturation signature");
    expect(snapshot.tciSuppressedCallbacks == 2
               && snapshot.suppressedBufferPeakBytes == 8192,
           "summary reports suppressed callback count and peak unread bytes");
    expect(snapshot.idleDuringTciTransitions == 1,
           "summary counts one Active-to-Idle transition during TCI");
    expect(snapshot.postTciLocalTxWhileIdle == 2,
           "summary counts every post-TCI stalled local TX attempt");
    expect(snapshot.lastMicReadAgeMs == 975,
           "summary reports age of the last successful microphone read");

    std::printf("\n%d of %d TX capture health tests failed.\n", g_failed, g_total);
    return g_failed == 0 ? 0 : 1;
}
