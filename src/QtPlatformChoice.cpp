#include "QtPlatformChoice.h"

namespace AetherSDR {

QString QtPlatformChoice::firstRequested(const QString& requested)
{
    const QString first = requested.section(QLatin1Char(';'), 0, 0).trimmed();
    return first.section(QLatin1Char(':'), 0, 0).trimmed();
}

// "wayland" loads as "wayland" or "wayland-egl", so a prefix match is a hit.
bool QtPlatformChoice::fellBack(const QString& requested, const QString& platformName)
{
    const QString first = firstRequested(requested);
    if (first.isEmpty() || platformName.isEmpty()) {
        return false;
    }
    return !platformName.startsWith(first, Qt::CaseInsensitive);
}

QStringList QtPlatformChoice::logLines(const QString& requested, QpaRequestSource source,
                                       const QString& platformName)
{
    QString origin;
    switch (source) {
        case QpaRequestSource::Unset:     origin = QStringLiteral("unset, Qt default"); break;
        case QpaRequestSource::User:      origin = QStringLiteral("set by the user"); break;
        case QpaRequestSource::AetherSDR: origin = QStringLiteral("set by AetherSDR"); break;
    }
    const QString value = source == QpaRequestSource::Unset ? QString() : requested;

    QStringList lines;
    lines << QStringLiteral("Platform: Qt platform plugin \"%1\" (QT_QPA_PLATFORM=%2, %3)")
                 .arg(platformName, value, origin);
    if (source != QpaRequestSource::Unset && fellBack(requested, platformName)) {
        lines << QStringLiteral("Platform: Qt fell back from \"%1\" to \"%2\"; "
                                "the first plugin in QT_QPA_PLATFORM did not load")
                     .arg(firstRequested(requested), platformName);
    }
    return lines;
}

} // namespace AetherSDR
