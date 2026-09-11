#pragma once

#include <QtGlobal>

namespace AetherSDR {

// Resolves a WSJT-X Status message's Rx/Tx frequencies from the dial
// frequency plus its audio offsets (#4526).
//
// WsjtxClient::parseStatus() reads DX Call / Report / Tx Mode / Tx Enabled /
// Transmitting / Decoding / Rx DF / Tx DF as one block, past the dial and
// mode this client already tracked for #3595. `hasOffsets` is false when any
// field in that block was short — an older WSJT-X schema, or a datagram
// truncated in flight. In that case none of the later fields, including Rx
// DF/Tx DF, are trusted: a partial read could pair a stale offset with the
// wrong field, which is a worse failure than simply not knowing an offset.
// Both frequencies then collapse to the dial itself rather than guessing —
// the same refuse-rather-than-guess shape WsjtxDialTracker uses for an
// unknown instance id.
struct WsjtxResolvedFreq {
    double rxFreqHz;
    double txFreqHz;
};

inline WsjtxResolvedFreq resolveWsjtxFreq(double dialFreqHz, bool hasOffsets,
                                          quint32 rxDfHz, quint32 txDfHz)
{
    if (!hasOffsets) {
        return {dialFreqHz, dialFreqHz};
    }
    // Same addition convention parseDecode() has always used for spot
    // placement: WSJT-X's audio-offset fields add to the dial unconditionally,
    // regardless of the reported sideband — WSJT-X always transmits digital
    // modes on the upper-sideband-equivalent audio convention.
    return {dialFreqHz + static_cast<double>(rxDfHz),
            dialFreqHz + static_cast<double>(txDfHz)};
}

}  // namespace AetherSDR
