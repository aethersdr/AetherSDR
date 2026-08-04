#pragma once

#include <QCoreApplication>
#include <QDir>
#include <QLibraryInfo>
#include <QString>
#include <QStringList>
#include <utility>

namespace AetherSDR {

// True when Qt's FFmpeg multimedia plugin is actually present on disk.
//
// This is the only honest signal we have for "custom QVideoFrameInput /
// QAudioBufferInput capture will work": the native WMF and AVFoundation
// backends advertise MP4/H.264/AAC encode just as happily as FFmpeg does, so
// QMediaFormat::isSupported() cannot discriminate between them. Inferring the
// answer from QT_MEDIA_BACKEND cannot work either, because the application
// itself writes that variable — the check would only ever observe its own
// preference, never whether the plugin exists.
inline bool ffmpegMediaBackendAvailable()
{
    QStringList roots = QCoreApplication::libraryPaths();
    roots << QLibraryInfo::path(QLibraryInfo::PluginsPath);

    for (const QString& root : roots) {
        if (root.isEmpty()) {
            continue;
        }
        QDir dir(root + QStringLiteral("/multimedia"));
        if (!dir.exists()) {
            continue;
        }
        const QStringList entries = dir.entryList(QDir::Files);
        for (const QString& entry : entries) {
            if (entry.contains(QLatin1String("ffmpeg"), Qt::CaseInsensitive)) {
                return true;
            }
        }
    }
    return false;
}

} // namespace AetherSDR
