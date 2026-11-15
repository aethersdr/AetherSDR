#pragma once

// Independent receive fixture: fixed AX.25 bytes, CRC-16/X25, HDLC bit stuffing
// and NRZI Bell 202. It does not call the application's TX/modulator APIs.
#include <QByteArray>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace AetherSDR::test::ax25rx {

inline QByteArray frame()
{
    return QByteArray::fromHex(
        "82a0a4a64040609c60868298987303f0"
        "21343734322e30304e2f31323231372e3030573e4f46464c494e4520415052532052582046495854555245");
}

inline unsigned fcs(const QByteArray& bytes)
{
    unsigned crc = 0xffff;
    for (unsigned char byte : bytes) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ ((crc & 1) ? 0x8408 : 0);
        }
    }
    return crc ^ 0xffff;
}

inline QVector<int> hdlcBits(int preambleFlags = 100, int postambleFlags = 32,
                             const QByteArray& payload = frame())
{
    QVector<int> bits;
    const auto byte = [&](unsigned value) {
        for (int bit = 0; bit < 8; ++bit) {
            bits.append((value >> bit) & 1);
        }
    };
    for (int flag = 0; flag < preambleFlags; ++flag) {
        byte(0x7e);
    }
    QByteArray bytes = payload;
    const unsigned crc = fcs(bytes);
    bytes.append(static_cast<char>(crc & 255));
    bytes.append(static_cast<char>(crc >> 8));
    int ones = 0;
    for (unsigned char value : bytes) {
        for (int index = 0; index < 8; ++index) {
            const int bit = (value >> index) & 1;
            bits.append(bit);
            if (bit) {
                if (++ones == 5) {
                    bits.append(0);
                    ones = 0;
                }
            } else {
                ones = 0;
            }
        }
    }
    for (int flag = 0; flag < postambleFlags; ++flag) {
        byte(0x7e);
    }
    return bits;
}

inline QVector<float> afsk(int sampleRate, int channels = 1, int preambleFlags = 100,
                           const QByteArray& payload = frame())
{
    const QVector<int> bits = hdlcBits(preambleFlags, 32, payload);
    QVector<float> samples;
    samples.reserve(bits.size() * (sampleRate / 1200) * channels);
    bool mark = true;
    double phase = 0;
    for (int bit : bits) {
        if (!bit) {
            mark = !mark;
        }
        const double step = 2 * std::numbers::pi * (mark ? 1200 : 2200) / sampleRate;
        for (int index = 0; index < sampleRate / 1200; ++index) {
            const float sample = static_cast<float>(0.5 * std::sin(phase));
            phase = std::fmod(phase + step, 2 * std::numbers::pi);
            samples.append(sample);
            if (channels == 2) {
                samples.append(sample * 0.75f);
            }
        }
    }
    return samples;
}

inline QByteArray cu8FmIq(int sampleRate = 2400000, double deviationHz = 2500,
                         double carrierOffsetHz = 0, double warmupSeconds = 0.35,
                         int preambleFlags = 160)
{
    const QVector<float> audio = afsk(sampleRate, 1, preambleFlags);
    const qsizetype warmup = static_cast<qsizetype>(warmupSeconds * sampleRate);
    QByteArray iq(2 * (warmup + audio.size()), Qt::Uninitialized);
    double phase = 0;
    for (qsizetype index = 0; index < warmup + audio.size(); ++index) {
        const double modulation = index < warmup ? 0 : 2 * audio[index - warmup];
        phase = std::remainder(phase + 2 * std::numbers::pi
            * (carrierOffsetHz + deviationHz * modulation) / sampleRate,
            2 * std::numbers::pi);
        iq[2 * index] = static_cast<char>(std::clamp(
            std::lround(127.5 + 38.4 * std::cos(phase)), 0L, 255L));
        iq[2 * index + 1] = static_cast<char>(std::clamp(
            std::lround(127.5 + 38.4 * std::sin(phase)), 0L, 255L));
    }
    return iq;
}

} // namespace AetherSDR::test::ax25rx
