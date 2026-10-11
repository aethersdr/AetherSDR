#include "QtPlatformChoice.h"

#include <QRegularExpression>

#include <cstring>

namespace AetherSDR {

QString QtPlatformChoice::firstRequested(const QString& requested)
{
    const QString first = requested.section(QLatin1Char(';'), 0, 0).trimmed();
    return first.section(QLatin1Char(':'), 0, 0).trimmed();
}

// "wayland" loads as "wayland" or "wayland-egl", and "wayland-egl" reports
// itself as "wayland", so a prefix match either way is a hit. The cost: a list
// such as "minimalegl;minimal" landing on "minimal" is not reported.
bool QtPlatformChoice::fellBack(const QString& requested, const QString& platformName)
{
    const QString first = firstRequested(requested);
    if (first.isEmpty() || platformName.isEmpty()) {
        return false;
    }
    return !platformName.startsWith(first, Qt::CaseInsensitive)
        && !first.startsWith(platformName, Qt::CaseInsensitive);
}

// Mirrors QGuiApplicationPrivate's argument loop: one leading '-' of "--" is
// dropped, and "-platform" takes the next argument when there is one.
std::optional<QString> QtPlatformChoice::platformArgument(int argc, const char* const* argv)
{
    std::optional<QString> value;
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (!arg || arg[0] != '-') {
            continue;
        }
        if (arg[1] == '-') {
            ++arg;
        }
        if (std::strcmp(arg, "-platform") == 0 && i + 1 < argc && argv[i + 1]) {
            value = QString::fromLocal8Bit(argv[++i]);
        }
    }
    return value;
}

QStringList QtPlatformChoice::logLines(const QString& rawRequested, QpaRequestSource source,
                                       const QString& platformName)
{
    // The request comes from argv or the environment: a CR or LF in it would
    // write a second, forged line into the log the Log Analyzer reads.
    static const QRegularExpression newlines(QStringLiteral("[\\r\\n]+"));
    const QString requested = QString(rawRequested).replace(newlines, QStringLiteral(" "));
    QStringList lines;
    if (source == QpaRequestSource::CommandLine) {
        lines << QStringLiteral("Platform: Qt platform plugin \"%1\" (-platform %2, on the command line)")
                     .arg(platformName, requested);
    } else {
        QString request;
        switch (source) {
            case QpaRequestSource::Unset:
                request = QStringLiteral("QT_QPA_PLATFORM unset, Qt default");
                break;
            case QpaRequestSource::User:
                request = QStringLiteral("QT_QPA_PLATFORM=%1, set by the user").arg(requested);
                break;
            case QpaRequestSource::AetherSDR:
                request = QStringLiteral("QT_QPA_PLATFORM=%1, set by AetherSDR").arg(requested);
                break;
            case QpaRequestSource::CommandLine:
                break;
        }
        lines << QStringLiteral("Platform: Qt platform plugin \"%1\" (%2)").arg(platformName, request);
    }
    if (source != QpaRequestSource::Unset && fellBack(requested, platformName)) {
        lines << QStringLiteral("Platform: Qt fell back from \"%1\" to \"%2\"; "
                                "the first plugin in %3 did not load")
                     .arg(firstRequested(requested), platformName,
                          source == QpaRequestSource::CommandLine
                              ? QStringLiteral("-platform")
                              : QStringLiteral("QT_QPA_PLATFORM"));
    }
    return lines;
}

} // namespace AetherSDR
