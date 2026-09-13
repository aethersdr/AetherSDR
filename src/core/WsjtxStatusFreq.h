#pragma once

#include "WsjtxWireHelpers.h"

#include <QDataStream>
#include <QString>
#include <QtGlobal>

namespace AetherSDR {

// The Status fields between Mode and Tx DF: DX Call, Report, Tx Mode,
// Tx Enabled, Transmitting, Decoding, Rx DF, Tx DF (#4526 RFC review item 1 —
// pulled out of WsjtxClient::parseStatus() specifically so the field ORDER
// is covered by a socket-free unit test, not just exercised manually. Getting
// this wrong — e.g. skipping straight from Mode to Tx Enabled without
// consuming DX Call/Report/Tx Mode first — fails silently: every later field
// reads garbage, hasOffsets ends up false, and resolveWsjtxFreq() quietly
// falls back to the dial. That's a green line sitting exactly on the dial
// with no error anywhere, which is why the parsing order itself needs a
// test, not just the addition math below).
struct WsjtxStatusBody {
    bool    hasOffsets{false};
    bool    transmitting{false};
    quint32 rxDfHz{0};
    quint32 txDfHz{0};
};

// Everything after Tx DF (DE call/grid, Tx Watchdog, Sub-mode, ...) is not
// needed here and is left unread — the caller stops consuming the stream
// once this returns, exactly as WsjtxClient::parseStatus() always has.
inline WsjtxStatusBody parseWsjtxStatusBody(QDataStream& ds)
{
    WsjtxStatusBody body;
    QString dxCall, report, txMode;
    bool txEnabled = false;
    bool decoding = false;
    const bool ok =
        wsjtxReadQString(ds, dxCall) &&
        wsjtxReadQString(ds, report) &&
        wsjtxReadQString(ds, txMode) &&
        wsjtxReadBool(ds, txEnabled) &&
        wsjtxReadBool(ds, body.transmitting) &&
        wsjtxReadBool(ds, decoding);

    if (ok && !ds.atEnd()) {
        quint32 rxDf = 0, txDf = 0;
        ds >> rxDf >> txDf;
        body.rxDfHz = rxDf;
        body.txDfHz = txDf;
        body.hasOffsets = true;
    }
    return body;
}

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
