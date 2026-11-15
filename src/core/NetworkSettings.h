#pragma once

#include "AppSettings.h"

#include <QJsonObject>
#include <QJsonDocument>
#include <algorithm>

namespace AetherSDR {

// Persistent network-tuning settings, stored as nested JSON under the single
// AppSettings "Network" key (Principle V — no new flat keys). Currently holds
// the VITA-49 UDP receive-buffer (SO_RCVBUF) request; extend the object for
// future network knobs rather than adding sibling flat keys.
class NetworkSettings {
public:
    // SO_RCVBUF request bounds for the VITA-49 stream socket. The slider snaps
    // to the presets below; the kernel additionally clamps the granted size at
    // net.core.rmem_max (surfaced via PanadapterStream::receiveBufferApplied).
    static constexpr int kDefaultRcvBufBytes = 4 * 1024 * 1024; // 4 MiB
    static constexpr int kMinRcvBufBytes     = 256 * 1024;      // 256 KiB
    static constexpr int kMaxRcvBufBytes     = 4 * 1024 * 1024; // 4 MiB

    static int vitaReceiveBufferBytes()
    {
        const int v = readObj()
                          .value(QStringLiteral("vitaReceiveBufferBytes"))
                          .toInt(kDefaultRcvBufBytes);
        return std::clamp(v, kMinRcvBufBytes, kMaxRcvBufBytes);
    }
    static void setVitaReceiveBufferBytes(int bytes)
    {
        QJsonObject o = readObj();
        o[QStringLiteral("vitaReceiveBufferBytes")] =
            std::clamp(bytes, kMinRcvBufBytes, kMaxRcvBufBytes);
        write(o);
    }

private:
    static QJsonObject readObj()
    {
        const QString json =
            AppSettings::instance().value(QStringLiteral("Network"), QString{}).toString();
        if (json.isEmpty())
            return {};
        return QJsonDocument::fromJson(json.toUtf8()).object();
    }
    static void write(const QJsonObject& o)
    {
        auto& s = AppSettings::instance();
        s.setValue(QStringLiteral("Network"),
                   QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)));
        s.save();
    }
};

} // namespace AetherSDR
