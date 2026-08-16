#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>

namespace AetherSDR {

class AudioEngine;

namespace DeviceDiagnostics {

struct AudioBusType {
    QString type;
    QString source;
};

struct TxAudioResamplingRoute {
    bool active{false};
    bool voiceInputNormalizingTo48k{false};
    bool voiceEgressResamplingTo24k{false};
    bool radeResamplingTo24k{false};
};

// Mirrors AudioEngine::onTxAudioReady() route priority without requiring an
// audio device in diagnostics tests. RADE consumes the mic buffer first, DAX
// bypasses it, and normal voice always performs the 48 -> 24 kHz egress SRC.
inline constexpr TxAudioResamplingRoute txAudioResamplingRoute(
    bool radeMode,
    bool daxTxMode,
    bool voiceInputNeedsNormalization,
    bool radeInputNeedsResampling) noexcept
{
    if (radeMode) {
        return {
            .active = radeInputNeedsResampling,
            .radeResamplingTo24k = radeInputNeedsResampling,
        };
    }
    if (daxTxMode) {
        return {};
    }
    return {
        .active = true,
        .voiceInputNormalizingTo48k = voiceInputNeedsNormalization,
        .voiceEgressResamplingTo24k = true,
    };
}

inline AudioBusType inferAudioBusType(const QString& description, const QByteArray& id)
{
    const QString text = (description + QLatin1Char(' ') + QString::fromLatin1(id)).toLower();

    if (text.contains(QStringLiteral("bluetooth"))
        || text.contains(QStringLiteral("airpods"))
        || text.contains(QStringLiteral(" bt "))
        || text.startsWith(QStringLiteral("bt "))
        || text.endsWith(QStringLiteral(" bt"))
        || text.contains(QStringLiteral("bth"))
        || text.contains(QStringLiteral("hfp"))
        || text.contains(QStringLiteral("sco"))) {
        return {QStringLiteral("Bluetooth"), QStringLiteral("name/id heuristic")};
    }

    if (text.contains(QStringLiteral("usb"))
        || text.contains(QStringLiteral("vid_"))
        || text.contains(QStringLiteral("pid_"))
        || text.contains(QStringLiteral("vid:"))
        || text.contains(QStringLiteral("pid:"))) {
        return {QStringLiteral("USB"), QStringLiteral("name/id heuristic")};
    }

    return {QStringLiteral("Unknown"), QStringLiteral("not exposed by Qt")};
}

QJsonObject buildAudioDevicesSnapshot(const AudioEngine* audio, const QJsonObject& snapshot);
QJsonObject buildAudioStartupSnapshot(const AudioEngine* audio, const QJsonObject& snapshot);
void annotateSliceAudioRoutes(QJsonObject* snapshot, const QJsonObject& audioDevices);

} // namespace DeviceDiagnostics
} // namespace AetherSDR
