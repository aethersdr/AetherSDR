#pragma once

#include <QFile>
#include <QString>
#include <QStringList>

namespace AetherSDR {

// The directories udev reads rules from, highest precedence first. A file
// in an earlier directory masks one of the same name in every later one.
inline QStringList udevRuleDirs()
{
    return {QStringLiteral("/etc/udev/rules.d"), QStringLiteral("/run/udev/rules.d"),
            QStringLiteral("/usr/local/lib/udev/rules.d"),
            QStringLiteral("/usr/lib/udev/rules.d"), QStringLiteral("/lib/udev/rules.d")};
}

// Whether the copy of `name` that udev actually uses (the first one found in
// `dirs`) is `rule`. A copy further down does not count: an older rule in
// /etc masks the packaged one in /usr/lib.
inline bool udevRuleInEffect(const QStringList& dirs, const QString& name, const QString& rule)
{
    for (const QString& dir : dirs) {
        QFile file(dir + QLatin1Char('/') + name);
        if (file.exists())
            return file.open(QIODevice::ReadOnly) && QString::fromUtf8(file.readAll()) == rule;
    }
    return false;
}

} // namespace AetherSDR
