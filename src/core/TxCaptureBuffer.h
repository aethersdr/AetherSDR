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

    // Did this read prove the ENDPOINT is producing audio? Bytes handed back
    // and bytes deliberately skipped to catch up both mean the device spoke;
    // whether the caller went on to USE them is a separate question. The
    // TCI-suppressed drain discards everything it reads and still answers yes,
    // which is what keeps the WASAPI silent-open watchdog off a working mic
    // (#2929, round-4 review of PR #5017).
    bool deliveredBytes() const { return !block.isEmpty() || discardedBytes > 0; }
};

// Reads at most kMaxReadBytes from a pull-mode capture device, first skipping
// any residue past kCatchUpThresholdBytes so the block returned is the newest
// audio the backend holds rather than the oldest.
// bytesPerSample is the NEGOTIATED device sample width (2 for Int16, 4 for
// Float32). Frame alignment has to follow it: aligning float32 capture to
// int16 frames lands a trim mid-sample, which slips the interleave and turns
// into audio that reads as a DSP fault rather than a framing one.
BoundedRead readLatestBounded(QIODevice* device, int inputChannels, int bytesPerSample);

inline BoundedRead readLatestBoundedInt16(QIODevice* device, int inputChannels)
{
    return readLatestBounded(device, inputChannels, static_cast<int>(sizeof(qint16)));
}

// Same drop-to-latest policy for push mode, where there is no read to bound:
// macOS hands over everything the capture callback accumulated since the last
// poll, so a stalled audio thread produces one oversized block. Trims block in
// place to its newest frame-aligned kMaxReadBytes and returns the bytes
// dropped (zero when it already fits).
qint64 trimToLatestBounded(QByteArray& block, int inputChannels, int bytesPerSample);

inline qint64 trimToLatestBoundedInt16(QByteArray& block, int inputChannels)
{
    return trimToLatestBounded(block, inputChannels, static_cast<int>(sizeof(qint16)));
}

} // namespace AetherSDR::TxCaptureBuffer
