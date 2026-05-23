#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>

namespace MasterSDR {

class AudioEngine;

namespace DeviceDiagnostics {

struct AudioBusType {
    QString type;
    QString source;
};

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
void annotateSliceAudioRoutes(QJsonObject* snapshot, const QJsonObject& audioDevices);

} // namespace DeviceDiagnostics
} // namespace MasterSDR
