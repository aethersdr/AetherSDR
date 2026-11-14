#pragma once

#include <QByteArray>
#include <QDataStream>
#include <QString>

namespace AetherSDR {

// Shared WSJT-X UDP wire primitives (#4526 RFC review item 1).
//
// Used by BOTH WsjtxClient's live parser and the socket-free field-walk in
// WsjtxStatusFreq.h, so a unit test exercises the exact same byte-level
// decoder production uses — not a second implementation that could quietly
// diverge from it. Big-endian, Qt-style length-prefixed UTF-8 strings
// (0xFFFFFFFF length = null string) and single-byte bools, matching WSJT-X's
// own QDataStream-based wire format.

inline bool wsjtxReadQString(QDataStream& ds, QString& out)
{
    if (ds.atEnd()) return false;
    quint32 len;
    ds >> len;
    if (len == 0xFFFFFFFF) {
        out.clear();
        return true;
    }
    if (len > 10000) return false;  // sanity check
    QByteArray buf(static_cast<int>(len), '\0');
    if (ds.readRawData(buf.data(), static_cast<int>(len)) != static_cast<int>(len))
        return false;
    out = QString::fromUtf8(buf);
    return true;
}

inline bool wsjtxReadBool(QDataStream& ds, bool& out)
{
    if (ds.atEnd()) return false;
    quint8 v;
    ds >> v;
    out = (v != 0);
    return true;
}

}  // namespace AetherSDR
