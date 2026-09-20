#pragma once

#include "PcmFrame.h"

#include <QByteArray>
#include <QtEndian>

#include <cstring>
#include <limits>
#include <optional>

namespace AetherSDR {

// RFC #5468 A3: one PCM16 stereo format selected before a file is published.
// The caller serializes selection with its RX observation and file lifecycle.
// An observation is metadata only: this class does not admit its PCM for writing.
class QsoRecordingFormat final {
public:
    explicit QsoRecordingFormat(const PcmFrame& observedRx = {})
        : m_sampleRateHz(selectRate(observedRx))
    {
    }

    // The rate a file may be STAMPED with is bounded by what this app can read
    // back, not merely by what a producer may emit. PcmFormat::valid() and
    // parseQsoWav()'s accepted set are separate literals that happen to agree
    // today; RFC #5468 is about adding rates, so widening one without the other
    // would publish a WAV AetherSDR then refuses to play. Fail to the legacy
    // rate instead of writing a file we cannot open.
    static int selectRate(const PcmFrame& observedRx)
    {
        if (!observedRx.current()
            || observedRx.stream().purpose != PcmPurpose::Speaker) {
            return 24000;
        }
        const int hz = observedRx.stream().format.sampleRateHz;
        return (hz == 24000 || hz == 48000) ? hz : 24000;
    }

    int sampleRateHz() const { return m_sampleRateHz; }
    int byteRate() const { return m_sampleRateHz * kBytesPerFrame; }

    static constexpr int kChannels = 2;
    static constexpr int kBitsPerSample = 16;
    static constexpr int kBytesPerFrame = 4;
    static constexpr int kHeaderBytes = 44;
    // RIFF's uint32 size includes the 36 non-data bytes after its size field.
    static constexpr quint64 kMaxDataBytes =
        (quint64{std::numeric_limits<quint32>::max()} - 36)
        / kBytesPerFrame * kBytesPerFrame;

    static bool validDataBytes(quint64 dataBytes)
    {
        return dataBytes <= kMaxDataBytes && dataBytes % kBytesPerFrame == 0;
    }

    std::optional<quint64> durationSecs(quint64 acceptedDataBytes) const
    {
        if (!validDataBytes(acceptedDataBytes)) {
            return std::nullopt;
        }
        return acceptedDataBytes / static_cast<quint64>(byteRate());
    }

    // Pure container encoding; the recorder owns exclusive creation, accepted
    // write accounting, header patch I/O and errors. Never relabel an open file.
    std::optional<QByteArray> wavHeader(quint64 acceptedDataBytes) const
    {
        if (!validDataBytes(acceptedDataBytes)) {
            return std::nullopt;
        }
        QByteArray header(kHeaderBytes, '\0');
        char* bytes = header.data();
        std::memcpy(bytes, "RIFF", 4);
        qToLittleEndian<quint32>(static_cast<quint32>(acceptedDataBytes + 36), bytes + 4);
        std::memcpy(bytes + 8, "WAVEfmt ", 8);
        qToLittleEndian<quint32>(16, bytes + 16);
        qToLittleEndian<quint16>(1, bytes + 20);
        qToLittleEndian<quint16>(kChannels, bytes + 22);
        qToLittleEndian<quint32>(m_sampleRateHz, bytes + 24);
        qToLittleEndian<quint32>(byteRate(), bytes + 28);
        qToLittleEndian<quint16>(kBytesPerFrame, bytes + 32);
        qToLittleEndian<quint16>(kBitsPerSample, bytes + 34);
        std::memcpy(bytes + 36, "data", 4);
        qToLittleEndian<quint32>(static_cast<quint32>(acceptedDataBytes), bytes + 40);
        return header;
    }

private:
    const int m_sampleRateHz;
};

} // namespace AetherSDR
