#pragma once

#include <QByteArray>

namespace AetherSDR {

// Per-audio-stream packet-loss concealment state.  Plain-data struct
// owned by PanadapterStream's per-stream map; mutated by
// applyConcealmentFade() as packets are processed.
struct AudioPlcState {
    int   lastFrames{0};       // stereo frames in last good packet
    int   pendingMissed{0};    // missed packets queued for concealment
    float tailL{0.0f};         // last emitted sample, used for fade-down
    float tailR{0.0f};
};

// Hard cap on consecutive concealed packets (~80 ms at 10 ms/pkt).  Past
// this, audio drops to clean silence rather than extending the synth.
constexpr int kMaxConcealPackets = 8;

// Prepend faded-silence concealment to float32 stereo PCM: cosine fade-down from
// the cached tail, zeros for the rest of the gap, cosine fade-up into the new
// head. Returns `pcm` unchanged when disabled or no loss is pending. Either way
// updates plc.tailL/R and plc.lastFrames from the output and resets
// plc.pendingMissed. (#2731)
QByteArray applyConcealmentFade(QByteArray pcm, AudioPlcState& plc,
                                bool enabled);

} // namespace AetherSDR
