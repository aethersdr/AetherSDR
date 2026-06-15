#pragma once

#include <QByteArray>
#include <QImage>
#include <QRgb>
#include <cstdint>

namespace AetherSDR {

// Decoder for the PA3FWM WebSDR waterfall stream (ws://<host>/~~waterstream<band>).
// We request format=1: each payload byte yields two palette-indexed pixels.
// 0xFF-prefixed frames are meta (scale reference / blanking / escaped literal).
// See websdr-client/PROTOCOL.md and docs/architecture/websdr-module-spec.md §1.2.
//
// Stateful only for the scale reference; rows are independent. One instance per
// stream on its worker thread.
class WebSdrWaterfallDecoder {
public:
    WebSdrWaterfallDecoder();

    void setWidth(int px) { m_width = (px > 0) ? px : 1; }
    int  width() const { return m_width; }

    // Decode one binary message. Returns true and fills `outRow` (a width×1
    // Format_RGB32 image) when the message is a waterfall row; false for meta
    // frames (which may update freqRef()).
    bool feed(const QByteArray& frame, QImage& outRow);

    quint32 freqRef() const { return m_freqRef; }

private:
    static QRgb paletteColor(int index);   // §1.2 formulas

    int     m_width{512};
    quint32 m_freqRef{0};
    QRgb    m_lut[256];
};

} // namespace AetherSDR
