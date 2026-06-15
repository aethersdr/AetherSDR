#include "WebSdrAudioDecoder.h"

#include <cmath>

namespace AetherSDR {

namespace {

// 256-entry a-law decode table, verbatim from websdr-sound.js.
const qint16 kAlaw[256] = {
-5504,-5248,-6016,-5760,-4480,-4224,-4992,-4736,-7552,-7296,-8064,-7808,-6528,-6272,-7040,-6784,
-2752,-2624,-3008,-2880,-2240,-2112,-2496,-2368,-3776,-3648,-4032,-3904,-3264,-3136,-3520,-3392,
-22016,-20992,-24064,-23040,-17920,-16896,-19968,-18944,-30208,-29184,-32256,-31232,-26112,-25088,-28160,-27136,
-11008,-10496,-12032,-11520,-8960,-8448,-9984,-9472,-15104,-14592,-16128,-15616,-13056,-12544,-14080,-13568,
-344,-328,-376,-360,-280,-264,-312,-296,-472,-456,-504,-488,-408,-392,-440,-424,
-88,-72,-120,-104,-24,-8,-56,-40,-216,-200,-248,-232,-152,-136,-184,-168,
-1376,-1312,-1504,-1440,-1120,-1056,-1248,-1184,-1888,-1824,-2016,-1952,-1632,-1568,-1760,-1696,
-688,-656,-752,-720,-560,-528,-624,-592,-944,-912,-1008,-976,-816,-784,-880,-848,
5504,5248,6016,5760,4480,4224,4992,4736,7552,7296,8064,7808,6528,6272,7040,6784,
2752,2624,3008,2880,2240,2112,2496,2368,3776,3648,4032,3904,3264,3136,3520,3392,
22016,20992,24064,23040,17920,16896,19968,18944,30208,29184,32256,31232,26112,25088,28160,27136,
11008,10496,12032,11520,8960,8448,9984,9472,15104,14592,16128,15616,13056,12544,14080,13568,
344,328,376,360,280,264,312,296,472,456,504,488,408,392,440,424,
88,72,120,104,24,8,56,40,216,200,248,232,152,136,184,168,
1376,1312,1504,1440,1120,1056,1248,1184,1888,1824,2016,1952,1632,1568,1760,1696,
688,656,752,720,560,528,624,592,944,912,1008,976,816,784,880,848};

// JS ToInt32 + bitwise ops on the 32-bit window.
inline qint32 i32(double x)  { return static_cast<qint32>(static_cast<quint32>(static_cast<qint64>(x))); }
inline qint32 i32(qint64 x)  { return static_cast<qint32>(static_cast<quint32>(x)); }
inline qint32 shl(qint32 a, int b) { return static_cast<qint32>(static_cast<quint32>(a) << (b & 31)); }
inline qint32 shr(qint32 a, int b) { return a >> (b & 31); }   // arithmetic

inline qint16 toInt16(double x) { return static_cast<qint16>(static_cast<qint64>(x)); }

// const lookup used by the predictor exponent selection
const int kC[8] = {999, 999, 8, 4, 2, 1, 99, 99};

} // namespace

WebSdrAudioDecoder::WebSdrAudioDecoder()
{
    resetPredictor();
}

void WebSdrAudioDecoder::resetPredictor()
{
    for (int i = 0; i < 20; ++i) { m_coeff[i] = 0.0; m_hist[i] = 0.0; }
    m_ie = 0.0;
}

QVector<qint16> WebSdrAudioDecoder::takeSamples()
{
    QVector<qint16> s = std::move(m_out);
    m_out.clear();
    return s;
}

void WebSdrAudioDecoder::feed(const QByteArray& frame)
{
    const quint8* t = reinterpret_cast<const quint8*>(frame.constData());
    const int L = frame.size();
    auto b = [t, L](int idx) -> int { return (idx >= 0 && idx < L) ? t[idx] : 0; };

    int n = 0;
    while (n < L) {
        bool decode = false;
        int  r = 0;
        const int byte = t[n];

        if ((byte & 0xF0) == 0xF0) {                       // S-meter
            m_smeter = ((byte & 15) << 8) + b(n + 1);
            n += 1;
        } else if (byte == 0x80) {                         // a-law block, 128 samples
            for (int s = 0; s < 128; ++s) {
                m_out.push_back(kAlaw[b(n + 1 + s)]);
            }
            n += 128;
            resetPredictor();
        } else if (byte >= 0x90 && byte <= 0xDF) {         // compressed audio
            r = 4;
            decode = true;
            m_re = 14 - (byte >> 4);
        } else if ((byte & 0x80) == 0) {                   // compressed audio (variant)
            r = 1;
            decode = true;
        } else if (byte == 0x81) {                         // sample-rate change
            m_D = (b(n + 1) << 8) + b(n + 2);
            if (m_D == 0) {
                m_stopped = true;
            }
            n += 2;
        } else if (byte == 0x82) {                         // quantiser scale
            m_oe = (b(n + 1) << 8) + b(n + 2);
            n += 2;
        } else if (byte == 0x83) {                         // filter/mode selection
            m_ae = b(n + 1);
            const int low = m_ae & 15;
            if (low != m_ee) {
                m_ee = low;
            }
            n += 1;
        } else if (byte == 0x84) {                         // 128 samples of silence
            for (int s = 0; s < 128; ++s) {
                m_out.push_back(0);
            }
            resetPredictor();
        } else if (byte == 0x85) {                         // true frequency (ignored value)
            n += 6;
        }
        // else: 0x86-0x8F, 0xE0-0xEF -> ignored

        if (decode) {
            decodeBlock(t, L, n, r);
            if (r == 0) {
                n -= 1;
            }
        }
        n += 1;
    }
}

void WebSdrAudioDecoder::decodeBlock(const quint8* t, int len, int& n, int& r)
{
    auto b = [t, len](int idx) -> int { return (idx >= 0 && idx < len) ? t[idx] : 0; };

    const int re = m_re;
    const int oe = m_oe;
    const int aShift = (m_ae & 16) ? 12 : 14;
    const bool dcReset = (m_ae & 16) != 0;

    for (int nsamp = 0; nsamp < 128; ++nsamp) {
        // 32-bit big-endian window at the current byte pointer, shifted by r
        qint32 win = static_cast<qint32>(
            (static_cast<quint32>(b(n + 3))      ) |
            (static_cast<quint32>(b(n + 2)) <<  8) |
            (static_cast<quint32>(b(n + 1)) << 16) |
            (static_cast<quint32>(b(n))     << 24));
        win = shl(win, r);

        int s = 0;
        if (win != 0) {
            const int u0 = 15 - re;
            while (win >= 0 && s < u0) { win = shl(win, 1); ++s; }
        }
        int u = 15 - re;
        if (s < u) {
            u = s;
            ++s;
            win = shl(win, 1);
        } else {
            u = shr(win, 24) & 0xFF;
            s += 8;
            win = shl(win, 8);
        }

        int f = 0;
        if (u >= kC[re]) {
            ++f;
        }
        if (u >= kC[re - 1]) {
            ++f;
        }
        if (f > re - 1) {
            f = re - 1;
        }

        qint32 part = shr((shr(win, 16) & 0xFFFF), 17 - re);
        qint32 c = part & i32(static_cast<qint64>(shl(-1, f)));
        c = c + shl(u, re - 1);
        if ((win & shl(1, 32 - re + f)) != 0) {
            c = c | ((1 << f) - 1);
            c = i32(static_cast<qint64>(~c));
        }

        r += s + re - f;
        while (r >= 8) { ++n; r -= 8; }

        // prediction
        double pred = 0.0;
        for (int k = 0; k < 20; ++k) {
            pred += m_coeff[k] * m_hist[k];
        }
        qint32 predi = i32(pred);
        qint32 predOut = (predi >= 0) ? (predi >> 12) : ((predi + 4095) >> 12);

        // dequantise residual
        double resid = static_cast<double>(c) * oe + oe / 2.0;
        c = shr(i32(resid), 4);

        // adapt coefficients (k = 19..0) and shift history (k = 19..1)
        for (int k = 19; k >= 0; --k) {
            m_coeff[k] += -(i32(m_coeff[k]) >> 7)
                        + (i32(m_hist[k] * static_cast<double>(c)) >> aShift);
            if (k == 0) {
                break;
            }
            m_hist[k] = m_hist[k - 1];
        }

        m_hist[0] = static_cast<double>(predOut) + resid;

        double sample = m_hist[0] + (i32(m_ie) >> 4);
        if (dcReset) {
            m_ie = 0.0;
        } else {
            m_ie = m_ie + shr(shl(i32(m_hist[0]), 4), 3);
        }

        m_out.push_back(toInt16(sample));
    }
}

} // namespace AetherSDR
