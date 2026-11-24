#pragma once

#include "core/backends/CwxStatusDelta.h"

#include <QChar>
#include <QMap>
#include <QString>
#include <QStringList>

namespace AetherSDR {

// A Flex "cwx …" status's key/values → CwxStatusDelta. Header-only so the
// lightweight CWX tests run the same parse.
inline CwxStatusDelta flexCwxStatus(const QMap<QString, QString>& kvs)
{
    // Read as CwxModel did.
    CwxStatusDelta d;
    for (auto it = kvs.cbegin(); it != kvs.cend(); ++it) {
        const QString& key = it.key();
        const QString& val = it.value();
        bool ok = false;
        if (key == QLatin1String("sent")) {
            const int v = val.toInt(&ok);
            if (ok) { d.sent = v; }
        } else if (key == QLatin1String("wpm")) {
            const int v = val.toInt(&ok);
            if (ok) { d.wpm = v; }
        } else if (key == QLatin1String("break_in_delay")) {
            const int v = val.toInt(&ok);
            if (ok) { d.breakInDelay = v; }
        } else if (key == QLatin1String("qsk_enabled")) {
            d.qsk = val == QLatin1String("1");
        } else if (key == QLatin1String("erase")) {
            const QStringList parts = val.split(QLatin1Char(','));
            bool ok1 = false;
            bool ok2 = false;
            if (parts.size() == 2) {
                const int start = parts[0].toInt(&ok1);
                const int stop = parts[1].toInt(&ok2);
                if (ok1 && ok2) { d.erase = std::make_pair(start, stop); }
            }
        } else if (key == QLatin1String("queue")) {
            d.queueDrained = val.isEmpty() || val == QLatin1String("0");
        } else if (key.startsWith(QLatin1String("macro")) && key.length() > 5) {
            const int index = key.mid(5).toInt(&ok);
            if (ok && index >= 1 && index <= 12) {
                // FlexLib CWX.cs substitutions for the quoted macro field.
                QString decoded = val;
                if (decoded.startsWith(QLatin1Char('"')) && decoded.endsWith(QLatin1Char('"'))) {
                    decoded = decoded.mid(1, decoded.length() - 2);
                }
                decoded.replace(QChar(0x7f), QLatin1Char(' '));
                decoded.replace(QChar(0x1e), QLatin1Char('"'));
                decoded.replace(QLatin1Char('*'), QLatin1Char('='));
                d.macros.insert(key, std::make_pair(index - 1, decoded));
            }
        }
    }
    return d;
}

} // namespace AetherSDR
