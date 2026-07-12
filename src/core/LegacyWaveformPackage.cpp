#include "LegacyWaveformPackage.h"
#include "ZipArchive.h"

#include <QFile>

#include <utility>

namespace AetherSDR {

namespace {

constexpr qsizetype kMaxConfigBytes = 256 * 1024;
constexpr qsizetype kMaxPackageEntries = 1024;
constexpr quint64 kMaxEntryBytes = 32ULL * 1024ULL * 1024ULL;
constexpr quint64 kMaxExpandedPackageBytes = 64ULL * 1024ULL * 1024ULL;

bool isSafeZipPath(const QString& path)
{
    if (path.isEmpty()
        || path.startsWith(QLatin1Char('/'))
        || path.contains(QStringLiteral(".."))
        || path.contains(QLatin1Char('\\'))) {
        return false;
    }
    return true;
}

QString stripQuotes(QString value)
{
    value = value.trimmed();
    if (value.size() >= 2
        && ((value.front() == QLatin1Char('"') && value.back() == QLatin1Char('"'))
            || (value.front() == QLatin1Char('\'') && value.back() == QLatin1Char('\'')))) {
        value = value.mid(1, value.size() - 2);
    }
    return value.trimmed();
}

QMap<QString, QString> parseHeaderFields(const QByteArray& config)
{
    QMap<QString, QString> fields;
    bool inHeader = false;
    const QList<QByteArray> lines = config.split('\n');
    for (QByteArray lineBytes : lines) {
        QString line = QString::fromUtf8(lineBytes).trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#'))
            || line.startsWith(QLatin1Char(';'))) {
            continue;
        }
        if (line.startsWith(QLatin1Char('[')) && line.endsWith(QLatin1Char(']'))) {
            const QString section = line.mid(1, line.size() - 2).trimmed().toLower();
            inHeader = section == QLatin1String("header");
            if (!inHeader && !fields.isEmpty()) {
                break;
            }
            continue;
        }
        if (!inHeader) {
            continue;
        }
        const int colon = line.indexOf(QLatin1Char(':'));
        if (colon <= 0) {
            continue;
        }
        const QString key = line.left(colon).trimmed().toLower();
        const QString value = stripQuotes(line.mid(colon + 1));
        if (!key.isEmpty()) {
            fields.insert(key, value);
        }
    }
    return fields;
}

LegacyWaveformPackageInfo invalidPackage(QString message)
{
    LegacyWaveformPackageInfo info;
    info.error = std::move(message);
    return info;
}

} // namespace

LegacyWaveformPackageInfo inspectLegacyWaveformPackage(const QByteArray& packageData)
{
    QString zipError;
    const ZipReadLimits limits{
        kMaxPackageEntries,
        kMaxEntryBytes,
        kMaxExpandedPackageBytes,
    };
    const QMap<QString, QByteArray> entries = readZipEntries(packageData,
                                                             &zipError,
                                                             limits);
    if (!zipError.isEmpty()) {
        return invalidPackage(zipError);
    }
    if (entries.isEmpty()) {
        return invalidPackage(QStringLiteral("Legacy waveform package is empty."));
    }

    LegacyWaveformPackageInfo info;
    for (auto it = entries.cbegin(); it != entries.cend(); ++it) {
        const QString path = it.key();
        if (!isSafeZipPath(path)) {
            return invalidPackage(QStringLiteral("Legacy waveform package contains an unsafe path: %1").arg(path));
        }
        info.entries.append(path);
    }
    info.entries.sort();

    QStringList configPaths;
    for (auto it = entries.cbegin(); it != entries.cend(); ++it) {
        const QString path = it.key();
        if (path.endsWith(QStringLiteral(".cfg"), Qt::CaseInsensitive)) {
            configPaths.append(path);
        }
    }
    if (configPaths.isEmpty()) {
        return invalidPackage(QStringLiteral("Legacy waveform package does not contain a .cfg manifest."));
    }
    if (configPaths.size() > 1) {
        return invalidPackage(QStringLiteral("Legacy waveform package contains multiple .cfg manifests."));
    }

    info.configPath = configPaths.constFirst();
    const int slash = info.configPath.lastIndexOf(QLatin1Char('/'));
    if (slash <= 0) {
        return invalidPackage(QStringLiteral("Legacy waveform manifest must live under a package directory."));
    }
    info.rootDirectory = info.configPath.left(slash);

    const QByteArray config = entries.value(info.configPath);
    if (config.isEmpty()) {
        return invalidPackage(QStringLiteral("Legacy waveform manifest is empty."));
    }
    if (config.size() > kMaxConfigBytes) {
        return invalidPackage(QStringLiteral("Legacy waveform manifest is too large."));
    }

    const QMap<QString, QString> header = parseHeaderFields(config);
    info.waveformName = header.value(QStringLiteral("name"));
    info.version = header.value(QStringLiteral("version"));
    info.executableName = header.value(QStringLiteral("executable"));

    if (info.waveformName.isEmpty()) {
        return invalidPackage(QStringLiteral("Legacy waveform manifest is missing header Name."));
    }
    if (info.executableName.isEmpty()) {
        return invalidPackage(QStringLiteral("Legacy waveform manifest is missing header Executable."));
    }
    if (info.executableName.contains(QLatin1Char('/'))
        || info.executableName.contains(QLatin1Char('\\'))
        || info.executableName.contains(QStringLiteral(".."))) {
        return invalidPackage(QStringLiteral("Legacy waveform executable name is unsafe."));
    }

    info.executablePath = info.rootDirectory + QLatin1Char('/') + info.executableName;
    if (!entries.contains(info.executablePath)) {
        return invalidPackage(QStringLiteral("Legacy waveform executable is missing: %1").arg(info.executablePath));
    }
    if (entries.value(info.executablePath).isEmpty()) {
        return invalidPackage(QStringLiteral("Legacy waveform executable is empty: %1").arg(info.executablePath));
    }

    info.valid = true;
    return info;
}

LegacyWaveformPackageInfo inspectLegacyWaveformPackageFile(const QString& filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return invalidPackage(QStringLiteral("Cannot open waveform package: %1").arg(file.errorString()));
    }
    return inspectLegacyWaveformPackage(file.readAll());
}

} // namespace AetherSDR
