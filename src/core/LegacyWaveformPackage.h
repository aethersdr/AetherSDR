#pragma once

#include <QByteArray>
#include <QMap>
#include <QString>
#include <QStringList>

namespace AetherSDR {

struct LegacyWaveformPackageInfo {
    bool valid{false};
    QString error;
    QString rootDirectory;
    QString configPath;
    QString executableName;
    QString executablePath;
    QString waveformName;
    QString version;
    QStringList entries;
};

LegacyWaveformPackageInfo inspectLegacyWaveformPackage(const QByteArray& packageData);
LegacyWaveformPackageInfo inspectLegacyWaveformPackageFile(const QString& filePath);

} // namespace AetherSDR
