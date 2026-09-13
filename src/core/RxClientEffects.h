#pragma once

#include "ClientComp.h"
#include "ClientDeEss.h"
#include "ClientEq.h"
#include "ClientGate.h"
#include "ClientPudu.h"
#include "ClientTube.h"

namespace AetherSDR {

// One source's client RX effect histories. The UI continues to own the main
// source's parameter objects; auxiliary sources copy those atomic parameters,
// never the filters, envelopes, lookahead samples or meter state. In particular,
// a 24 kHz Kiwi stream must not advance the main producer's 48 kHz effect state.
//
// Construct/prepare before processing. All other calls belong to the source's
// audio owner, while syncParametersFrom() may read UI-updated parameter objects.
// Neither synchronization nor processing allocates or locks. The caller retains
// the existing stage order and post-EQ tap through the individual accessors.
class RxClientEffects {
public:
    explicit RxClientEffects(int sampleRate = 24000);

    void syncParametersFrom(const ClientEq& eq, const ClientGate& gate,
                            const ClientComp& comp, const ClientDeEss& deEss,
                            const ClientTube& tube, const ClientPudu& pudu) noexcept;
    void reset() noexcept;

    ClientEq& eq() noexcept { return m_eq; }
    ClientGate& gate() noexcept { return m_gate; }
    ClientComp& comp() noexcept { return m_comp; }
    ClientDeEss& deEss() noexcept { return m_deEss; }
    ClientTube& tube() noexcept { return m_tube; }
    ClientPudu& pudu() noexcept { return m_pudu; }

private:
    ClientEq m_eq;
    ClientGate m_gate;
    ClientComp m_comp;
    ClientDeEss m_deEss;
    ClientTube m_tube;
    ClientPudu m_pudu;
};

} // namespace AetherSDR
