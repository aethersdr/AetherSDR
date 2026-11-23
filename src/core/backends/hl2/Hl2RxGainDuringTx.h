#pragma once

// The receiver's AD9866 gain while the HL2 transmits. The gateware switches it
// at the T/R edge from register 0x0e (ad9866.v, FAST_LNA block); the host only
// says which value. Always on, no setting: see docs/HERMES.md §13 row 22.

namespace AetherSDR::hl2 {

// Ceiling on the receive gain while keyed, dB. The TX leak into RX1 on 40 m
// was -31.5 dBFS at LNA 20 and 0.37 W (linear) and pinned near -10.6 dBFS at
// LNA 48 (~7 dB compressed). At 5 W (+11.3 dB) an 8 dB gain puts the leak at
// about -32 dBFS: no hotter than the strongest level measured linear, and
// ~21 dB under the compressed one. A gain at or below it is left alone.
inline constexpr int kRxGainDuringTxCapDb = 8;

// True when the cap is below the receive gain, so 0x0e must carry it. With
// PureSignal on, the keyed 0x0a bank owns the gain and 0x0e stays inert.
constexpr bool rxGainDuringTxEngaged(int rxGainDb, bool pureSignal) noexcept
{
    return !pureSignal && rxGainDb > kRxGainDuringTxCapDb;
}

// The gain the AD9866 runs at while keyed, for a given receive gain.
constexpr int rxGainDuringTxDb(int rxGainDb, bool pureSignal) noexcept
{
    return rxGainDuringTxEngaged(rxGainDb, pureSignal) ? kRxGainDuringTxCapDb : rxGainDb;
}

// dB the keyed receiver sits below the receive gain; the display reference
// adds it back so levels stay antenna-referred while transmitting.
constexpr int rxGainDuringTxShortfallDb(int rxGainDb, bool pureSignal) noexcept
{
    return rxGainDb - rxGainDuringTxDb(rxGainDb, pureSignal);
}

}  // namespace AetherSDR::hl2
