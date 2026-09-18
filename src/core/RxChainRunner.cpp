#include "RxChainRunner.h"

#include "AudioEngine.h"
#include "ClientComp.h"
#include "ClientEq.h"
#include "ClientGate.h"
#include "ClientPudu.h"
#include "ClientTube.h"

namespace AetherSDR {

namespace {

// Every module exposes the same isEnabled()/process(float*, frames, channels)
// pair, so one helper covers all five.
template <class Module>
void applyStage(Module* module, QByteArray& scratch, bool bypass,
                const QByteArray*& current)
{
    if (!module || !module->isEnabled() || bypass) return;
    scratch = *current;
    const int frames = scratch.size() / (2 * static_cast<int>(sizeof(float)));
    if (frames <= 0) return;
    module->process(reinterpret_cast<float*>(scratch.data()), frames, 2);
    current = &scratch;
}

} // namespace

const QByteArray* runRxChain(uint64_t packed,
                             const QByteArray& input,
                             const RxChainModules& modules,
                             RxChainScratch& scratch,
                             bool bypass,
                             const QByteArray** postEq)
{
    const QByteArray* current = &input;
    if (postEq) *postEq = current;

    for (int slot = 0; slot < AudioEngine::kMaxRxChainStages; ++slot) {
        const auto stage = static_cast<AudioEngine::RxChainStage>(
            (packed >> (slot * 8)) & 0xFF);
        switch (stage) {
            case AudioEngine::RxChainStage::None:
                return current;               // end-of-list marker
            case AudioEngine::RxChainStage::Eq:
                applyStage(modules.eq, scratch.eq, bypass, current);
                // Reported whether or not the EQ ran: the tap shows what is
                // heading for the sink at this point in the chain, not
                // whether a stage was engaged.
                if (postEq) *postEq = current;
                break;
            case AudioEngine::RxChainStage::Gate:
                applyStage(modules.gate, scratch.gate, bypass, current);
                break;
            case AudioEngine::RxChainStage::Comp:
                applyStage(modules.comp, scratch.comp, bypass, current);
                break;
            case AudioEngine::RxChainStage::Tube:
                applyStage(modules.tube, scratch.tube, bypass, current);
                break;
            case AudioEngine::RxChainStage::Pudu:
                applyStage(modules.pudu, scratch.pudu, bypass, current);
                break;
        }
    }
    return current;
}

} // namespace AetherSDR
