#include "WebSdrWaterfallDecoder.h"

#include <cmath>

namespace AetherSDR {

QRgb WebSdrWaterfallDecoder::paletteColor(int e)
{
    // Verbatim from websdr-waterfall.js; each component stored into a uint8.
    int r, g, b;
    if (e < 64)        { r = 0;            g = 0;                                   b = 2 * e; }
    else if (e < 128)  { r = 3 * e - 192;  g = 0;                                   b = 2 * e; }
    else if (e < 192)  { r = e + 64;       g = int(256.0 * std::sqrt((e - 128) / 64.0)); b = 511 - 2 * e; }
    else               { r = 255;          g = 255;                                 b = 512 + 2 * e; }
    return qRgb(r & 0xFF, g & 0xFF, b & 0xFF);
}

WebSdrWaterfallDecoder::WebSdrWaterfallDecoder()
{
    for (int i = 0; i < 256; ++i) {
        m_lut[i] = paletteColor(i);
    }
}

bool WebSdrWaterfallDecoder::feed(const QByteArray& frame, QImage& outRow)
{
    const quint8* t = reinterpret_cast<const quint8*>(frame.constData());
    const int L = frame.size();
    if (L == 0) {
        return false;
    }

    int off = 0;
    if (t[0] == 0xFF) {
        if (L > 1 && t[1] != 0xFF) {
            // meta frame
            if (t[1] == 1 && L >= 7) {
                m_freqRef = static_cast<quint32>(t[3])
                          | (static_cast<quint32>(t[4]) << 8)
                          | (static_cast<quint32>(t[5]) << 16)
                          | (static_cast<quint32>(t[6]) << 24);
            }
            // 0xFF 0x02 (blank N leading pixels) is ignored for the mini view.
            return false;
        }
        off = 1;   // 0xFF 0xFF -> escaped literal: row data after one byte
    }

    outRow = QImage(m_width, 1, QImage::Format_RGB32);
    outRow.fill(qRgb(0, 0, 0));
    QRgb* px = reinterpret_cast<QRgb*>(outRow.bits());

    for (int j = off; j < L; ++j) {
        const int e = j - off;
        const int p0 = 2 * e;
        const int p1 = 2 * e + 1;
        if (p0 < m_width) {
            px[p0] = m_lut[(16 * (t[j] & 15) + 2) & 0xFF];
        }
        if (p1 < m_width) {
            px[p1] = m_lut[t[j] & 0xF2];
        } else {
            break;
        }
    }
    return true;
}

} // namespace AetherSDR
