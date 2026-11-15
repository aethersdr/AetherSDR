#pragma once

#include "core/backends/anan/P2Protocol.h"   // kMaxDdcs

#include <array>
#include <cstddef>

namespace AetherSDR::anan {

// Where each receiver sits on the radio: the DDC that carries it, and the ADC
// that feeds that DDC unless the operator picks the other input. This is the
// one place the layout is written down, so a per-radio profile can replace the
// table without touching the session code that reads it.
//
// The layout is the reference clients' for a Saturn board: receivers start at
// DDC2, leaving DDC0 and DDC1 free as the synchronised pair that diversity and
// PureSignal feedback use (deskHPSDR new_protocol.c update_action_table();
// Thetis console.cs UpdateDDCs()). Any DDC serves: the Saturn gateware builds
// every DDC from the same block (saturn_project.tcl, ten DDC_Block instances),
// so the six rates and the droop correction are the same on each.
struct ReceiverRoute {
    int ddc = 0;
    int adc = 0;   // 0 = ADC0 behind the Ant1/2/3 relays, 1 = ADC1 on the RX2 jack
};

inline constexpr std::array<ReceiverRoute, 1> kReceiverRoutes{{
    {2, 0},   // RX1
}};

inline constexpr ReceiverRoute kRx1Route = kReceiverRoutes[0];

namespace detail {
// Every route names a DDC the codec will encode and an ADC the radio has, and
// no two receivers share a DDC.
constexpr bool receiverRoutesValid()
{
    for (std::size_t i = 0; i < kReceiverRoutes.size(); ++i) {
        const ReceiverRoute& r = kReceiverRoutes[i];
        if (r.ddc < 0 || r.ddc >= kMaxDdcs || r.adc < 0 || r.adc > 1)
            return false;
        for (std::size_t j = 0; j < i; ++j) {
            if (kReceiverRoutes[j].ddc == r.ddc)
                return false;
        }
    }
    return true;
}
}  // namespace detail

static_assert(detail::receiverRoutesValid(),
              "kReceiverRoutes: a DDC outside 0..kMaxDdcs-1, an ADC other than 0/1, or a shared DDC");

}  // namespace AetherSDR::anan
