#pragma once

#include "core/CommandParser.h"
#include "core/StreamStatus.h"

#include <QMap>
#include <QString>

#include <algorithm>
#include <utility>

namespace MasterSDR::RadioStatusOwnership {

inline QString hexId(quint32 value)
{
    return QStringLiteral("0x%1")
        .arg(QString::number(value, 16).rightJustified(8, QLatin1Char('0')));
}

inline QString streamCommandId(quint32 value)
{
    return value == 0
        ? QString()
        : QString::number(value, 16).rightJustified(8, QLatin1Char('0'));
}

inline QString normalizedFlexId(QString text)
{
    text = text.trimmed();
    if (text.isEmpty()) {
        return QString();
    }

    if (text.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive)) {
        text = text.mid(2);
    }

    bool ok = false;
    const quint32 id = text.toUInt(&ok, 16);
    return ok ? hexId(id) : QString();
}

inline quint32 parseFlexId(QString text)
{
    return parseStatusHandle(std::move(text));
}

inline QString parsePanafallCreatePanId(const QString& body)
{
    const QMap<QString, QString> kvs = CommandParser::parseKVs(body);
    if (kvs.contains(QStringLiteral("pan"))) {
        return normalizedFlexId(kvs.value(QStringLiteral("pan")));
    }
    if (kvs.contains(QStringLiteral("id"))) {
        return normalizedFlexId(kvs.value(QStringLiteral("id")));
    }

    const QStringList parts = body.trimmed().split(QLatin1Char(','));
    return parts.isEmpty() ? QString() : normalizedFlexId(parts.constFirst());
}

struct StreamObjectParts {
    bool valid{false};
    quint32 streamId{0};
    QString action;
};

inline StreamObjectParts parseStreamObject(const QString& object,
                                           const QString& prefix = QStringLiteral("stream"))
{
    if (!object.startsWith(prefix + QLatin1Char(' ')))
        return {};

    const QString rest = object.mid(prefix.size() + 1).trimmed();
    const int firstSpace = rest.indexOf(QLatin1Char(' '));
    const QString idText = firstSpace >= 0 ? rest.left(firstSpace) : rest;

    StreamObjectParts parts;
    parts.streamId = parseFlexId(idText);
    parts.valid = parts.streamId != 0;
    if (firstSpace >= 0)
        parts.action = rest.mid(firstSpace + 1).trimmed();
    return parts;
}

inline bool statusRemoved(const StreamObjectParts& stream,
                          const QMap<QString, QString>& kvs)
{
    return stream.action == QStringLiteral("removed")
        || kvs.contains(QStringLiteral("removed"))
        || kvs.value(QStringLiteral("in_use")) == QStringLiteral("0");
}

enum class OwnedStatusAction {
    Defer,
    Claim,
    Apply,
    Ignore,
    Remove
};

inline OwnedStatusAction classifyOwnedStatus(bool alreadyOwned,
                                             const QMap<QString, QString>& kvs,
                                             bool removed,
                                             quint32 ourHandle)
{
    if (removed)
        return OwnedStatusAction::Remove;

    if (!kvs.contains(QStringLiteral("client_handle")))
        return alreadyOwned ? OwnedStatusAction::Apply : OwnedStatusAction::Defer;

    const quint32 owner = parseFlexId(kvs.value(QStringLiteral("client_handle")));
    if (owner == ourHandle)
        return alreadyOwned ? OwnedStatusAction::Apply : OwnedStatusAction::Claim;

    return OwnedStatusAction::Ignore;
}

struct RemoteAudioRxTracking {
    quint32 streamId{0};
    quint32 clientHandle{0};
    bool createPending{false};
    bool removeRequested{false};
    bool statusSeen{false};
    QString compression;
};

enum class RemoteAudioRxAction {
    NotRemoteAudio,
    DeferredUnknownOwner,
    IgnoredOtherClient,
    Adopted,
    Updated,
    Removed
};

inline quint32 parseCreateResponseStreamId(const QString& body)
{
    const auto parseStreamId = [](QString text) -> quint32 {
        text = text.trimmed();
        if (text.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive))
            text = text.mid(2);

        bool ok = false;
        const quint32 value = text.toUInt(&ok, 16);
        return ok ? value : 0;
    };

    const QMap<QString, QString> kvs = CommandParser::parseKVs(body);
    if (kvs.contains(QStringLiteral("stream")))
        return parseStreamId(kvs.value(QStringLiteral("stream")));
    if (kvs.contains(QStringLiteral("id")))
        return parseStreamId(kvs.value(QStringLiteral("id")));
    return parseStreamId(body);
}

inline int boundedSliceCapacity(int modelLimit,
                                int currentMax,
                                int currentSliceCount,
                                int reportedAvailableSlices)
{
    if (modelLimit <= 0) {
        return currentMax;
    }
    const int clampedCurrentMax = std::min(currentMax, modelLimit);

    if (reportedAvailableSlices < 0) {
        return clampedCurrentMax;
    }

    const int reportedTotal = currentSliceCount + reportedAvailableSlices;
    return std::min(modelLimit, std::max(clampedCurrentMax, reportedTotal));
}

inline RemoteAudioRxAction applyRemoteAudioRxStatus(RemoteAudioRxTracking& state,
                                                    const QString& object,
                                                    const QMap<QString, QString>& kvs,
                                                    quint32 ourHandle,
                                                    bool allowUnknownOwner)
{
    const StreamObjectParts stream = parseStreamObject(object);
    if (!stream.valid)
        return RemoteAudioRxAction::NotRemoteAudio;

    const QString incomingType = kvs.value(QStringLiteral("type"));
    const bool matchesKnownStream = state.streamId != 0 && stream.streamId == state.streamId;
    const bool isRemoteAudioRx = incomingType == QStringLiteral("remote_audio_rx")
        || matchesKnownStream;

    if (!isRemoteAudioRx)
        return RemoteAudioRxAction::NotRemoteAudio;

    const bool removed = statusRemoved(stream, kvs);
    if (removed) {
        if (matchesKnownStream) {
            state.streamId = 0;
            state.clientHandle = 0;
            state.createPending = false;
            state.removeRequested = false;
            state.statusSeen = false;
            state.compression.clear();
            return RemoteAudioRxAction::Removed;
        }
        return RemoteAudioRxAction::IgnoredOtherClient;
    }

    if (!kvs.contains(QStringLiteral("client_handle"))) {
        if (!matchesKnownStream && !allowUnknownOwner)
            return RemoteAudioRxAction::DeferredUnknownOwner;
    } else {
        const quint32 owner = parseFlexId(kvs.value(QStringLiteral("client_handle")));
        if (owner != 0 && owner != ourHandle)
            return RemoteAudioRxAction::IgnoredOtherClient;
        state.clientHandle = owner == 0 ? ourHandle : owner;
    }

    const bool adopted = state.streamId == 0 || state.streamId != stream.streamId;
    state.streamId = stream.streamId;
    if (state.clientHandle == 0)
        state.clientHandle = ourHandle;
    state.createPending = false;
    state.statusSeen = true;
    if (kvs.contains(QStringLiteral("compression")))
        state.compression = kvs.value(QStringLiteral("compression"));

    return adopted ? RemoteAudioRxAction::Adopted : RemoteAudioRxAction::Updated;
}

} // namespace MasterSDR::RadioStatusOwnership
