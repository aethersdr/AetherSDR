#pragma once

// Shared helpers for parsing SmartSDR stream-status fields.
// Deduplicated from MainWindow.cpp, TciServer.cpp, and RadioModel.cpp (#2142).

#include <QString>
#include <QMap>

namespace AetherSDR {

// Parse a hex-or-decimal handle/token string (e.g. "0x12345678" or "305419896")
// into a quint32.  Returns 0 on failure.
inline quint32 parseStatusHandle(QString text)
{
    text = text.trimmed();
    bool ok = false;
    quint32 value = text.toUInt(&ok, 0);
    if (ok)
        return value;

    if (text.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive))
        text = text.mid(2);
    value = text.toUInt(&ok, 16);
    return ok ? value : 0;
}

// Return true if a stream-status key-value map either omits client_handle
// (legacy firmware) or explicitly matches our own handle. Current firmware can
// report orphan streams as client_handle=0x00000000 ip=0.0.0.0; those are not
// usable client-owned streams. Treat owner=0 without that dead endpoint as
// legacy/unknown ownership for older firmware compatibility.
// The same rule on already-parsed fields: whether client_handle was reported,
// its parsed value, and the trimmed ip.
inline bool streamOwnerBelongsToUs(bool ownerReported, quint32 owner, const QString& ip,
                                   quint32 ourHandle)
{
    if (!ownerReported)
        return true; // Preserve compatibility with status lines that omit ownership.
    if (owner == 0)
        return ip != QStringLiteral("0.0.0.0");
    return owner == ourHandle;
}

inline bool streamStatusBelongsToUs(const QMap<QString, QString>& kvs, quint32 ourHandle)
{
    return streamOwnerBelongsToUs(kvs.contains(QStringLiteral("client_handle")),
                                  parseStatusHandle(kvs.value(QStringLiteral("client_handle"))),
                                  kvs.value(QStringLiteral("ip")).trimmed(), ourHandle);
}

inline bool isDeadOrphanStreamOwner(bool ownerReported, quint32 owner, const QString& ip)
{
    return ownerReported && owner == 0 && ip == QStringLiteral("0.0.0.0");
}

inline bool isDeadOrphanDaxRxStatus(const QMap<QString, QString>& kvs)
{
    return isDeadOrphanStreamOwner(kvs.contains(QStringLiteral("client_handle")),
                                   parseStatusHandle(kvs.value(QStringLiteral("client_handle"))),
                                   kvs.value(QStringLiteral("ip")).trimmed());
}

inline bool daxTxStatusCanUpdateLocalState(quint32 streamId,
                                           quint32 currentStreamId,
                                           const QMap<QString, QString>& kvs,
                                           quint32 ourHandle)
{
    if (streamId == currentStreamId && currentStreamId != 0)
        return true;

    if (!kvs.contains(QStringLiteral("client_handle")))
        return false;

    const quint32 owner = parseStatusHandle(kvs.value(QStringLiteral("client_handle")));
    return owner != 0 && owner == ourHandle;
}

} // namespace AetherSDR
