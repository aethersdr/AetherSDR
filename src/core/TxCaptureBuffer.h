#pragma once

#include <QByteArray>
#include <QtGlobal>

class QIODevice;

namespace AetherSDR::TxCaptureBuffer {

// QAudioSource normally delivers a fraction of its ~200 ms device buffer per
// readyRead. Keep a generous ceiling so a delayed audio-thread callback can
// catch up without allowing an unbounded backend residue to become one giant
// allocation. At 48 kHz stereo Int16 this is about 1.36 seconds of audio.
inline constexpr qint64 kMaxReadBytes = 256 * 1024;

// Above this much unread residue the backend is past catch-up distance:
// consuming it one bounded block per callback would put minutes of hours-stale
// microphone audio on the air, 1.36 s at a time, with nothing to tell the
// operator why. A realtime capture path wants the freshest block, so older
// residue is skipped without allocating it. Four blocks is about 5.5 s at
// 48 kHz stereo Int16 — well beyond any legitimate audio-thread scheduling
// delay, and short enough that no stale tail is audible.
inline constexpr qint64 kCatchUpThresholdBytes = 4 * kMaxReadBytes;

struct BoundedRead {
    // The freshest bounded block, frame-aligned for the negotiated channel
    // count. Empty when the device held less than one whole frame.
    QByteArray block;
    // Stale bytes skipped to catch up; zero on the normal path. Non-zero means
    // the backend outran the consumer and audio was dropped deliberately.
    qint64 discardedBytes{0};
};

// Reads at most kMaxReadBytes from a pull-mode capture device, first skipping
// any residue past kCatchUpThresholdBytes so the block returned is the newest
// audio the backend holds rather than the oldest.
BoundedRead readLatestBoundedInt16(QIODevice* device, int inputChannels);

// Same drop-to-latest policy for push mode, where there is no read to bound:
// macOS hands over everything the capture callback accumulated since the last
// poll, so a stalled audio thread produces one oversized block. Trims block in
// place to its newest frame-aligned kMaxReadBytes and returns the bytes
// dropped (zero when it already fits).
qint64 trimToLatestBoundedInt16(QByteArray& block, int inputChannels);

} // namespace AetherSDR::TxCaptureBuffer
