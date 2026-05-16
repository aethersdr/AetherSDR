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

// Prepend faded-silence concealment to a float32 stereo PCM buffer
// before emit.  Returns the (possibly enlarged) buffer with a cosine
// fade-down from the cached tail, zero-pad for the rest of the gap,
// and cosine fade-up into the newly received head.
//
// Pure function: state is captured in `plc`, `enabled` is passed in so
// the caller can flip behaviour at runtime without touching this
// translation unit.  Returns `pcm` unchanged when concealment is
// disabled or no loss is pending; otherwise returns a new QByteArray
// with concealment frames prepended to the input PCM.  In both cases
// `plc.tailL/R` and `plc.lastFrames` are updated to reflect the output's
// last samples and `plc.pendingMissed` is reset to 0.  (#2731)
QByteArray applyConcealmentFade(QByteArray pcm, AudioPlcState& plc,
                                bool enabled);

} // namespace AetherSDR
