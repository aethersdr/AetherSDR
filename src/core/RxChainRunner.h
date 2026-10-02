#pragma once

#include <QByteArray>

#include <cstdint>

namespace AetherSDR {

class ClientComp;
class ClientEq;
class ClientGate;
class ClientPudu;
class ClientTube;

// The receive chain, applied in the order the operator arranged it.
//
// Lives outside AudioEngine so it can be driven by a test with real PCM and
// real DSP modules: the order only matters if the samples coming out differ,
// and nothing that asserts on the stored vector can show that. Reordering was
// cosmetic for a long time precisely because the audio path hardcoded
// EQ → Gate → Comp → Tube → Pudu while the UI wrote a stored order that no
// processing code read.
struct RxChainModules {
    ClientEq*   eq{nullptr};
    ClientGate* gate{nullptr};
    ClientComp* comp{nullptr};
    ClientTube* tube{nullptr};
    ClientPudu* pudu{nullptr};
};

// One scratch buffer per stage, owned by the caller so the audio thread
// reuses the same allocations block after block.
struct RxChainScratch {
    QByteArray eq;
    QByteArray gate;
    QByteArray comp;
    QByteArray tube;
    QByteArray pudu;
};

// Apply each stage of `packed` (AudioEngine::RxChainStage bytes, low slot
// first, None-terminated). Returns `input` if every stage was skipped, else the
// last stage's scratch. `postEq`, if non-null, receives the buffer right after
// the EQ slot wherever it sits. Stages skip when absent, disabled, or `bypass`
// (TX). Allocates only when scratch first grows to block size.
const QByteArray* runRxChain(uint64_t packed,
                             const QByteArray& input,
                             const RxChainModules& modules,
                             RxChainScratch& scratch,
                             bool bypass,
                             const QByteArray** postEq = nullptr);

} // namespace AetherSDR
