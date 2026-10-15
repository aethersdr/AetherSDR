#pragma once

#include "core/backends/flex/CommandParser.h"
#include "core/StreamStatus.h"

#include <QMap>
#include <QString>
#include <QStringList>
#include <QStringView>

#include <algorithm>
#include <utility>

namespace AetherSDR::RadioStatusOwnership {

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

// Reconcile local TX audio/meters only after the radio reports TX. FlexLib
// 4.3.16 Radio.cs ParsePTTSource describes comma-separated latched sources.
// VOX still requires its enable flag: older firmware reports it as SW.
// Neither a source token nor this predicate grants an intent or keys TX.
inline bool interlockKeepsLocalTxOn(bool txOwnedByUs, bool txRequested,
                                    bool cwKeyActive, bool cwxActive, bool tuning,
                                    const QString& lastInterlockSource,
                                    bool voxEnabled)
{
    if (!txOwnedByUs) {
        return false;
    }
    if (txRequested || cwKeyActive || cwxActive || tuning || voxEnabled) {
        return true;
    }
    // Status retains this remote field across deltas. Scan views so repeated
    // comma-dense input cannot amplify allocations into one string per token.
    const QStringView sources(lastInterlockSource);
    qsizetype from = 0;
    for (;;) {
        const qsizetype comma = sources.indexOf(QLatin1Char(','), from);
        const qsizetype end = comma < 0 ? sources.size() : comma;
        const QStringView token = sources.mid(from, end - from);
        if (token == QLatin1String("MIC") || token == QLatin1String("ACC")
            || token == QLatin1String("RCA")) {
            return true;
        }
        if (comma < 0) {
            return false;
        }
        from = comma + 1;
    }
}

// Whether the *local operator* is the one keying the transmitter — mic/PTT,
// MOX, VOX, or footswitch — as opposed to a TUNE/two-tone/ATU carrier or a
// TCI-hardware/DAX (external-app) transmit. Drives the status-bar TX timer.
// The interlock reports both TCI-hardware PTT and DAX as source=SW, so the
// caller disambiguates them from the remembered PTT source. `sourceIsTuneCarrier`
// (tune and `atu start`, captured before dispatch) spans the whole interlock
// cycle. CW is excluded: QSK toggles the interlock per element.
inline bool operatorTransmitActive(bool transmitting, bool daxTxActive,
                                   bool sourceIsTciHardware, bool sourceIsDax,
                                   bool sourceIsTuneCarrier, bool modeIsCw)
{
    return transmitting && !daxTxActive && !sourceIsTciHardware && !sourceIsDax
           && !sourceIsTuneCarrier && !modeIsCw;
}

// A TGXL autotune keys the radio over its interlock line, so the PTT source
// can't identify it. AetherSDR arms a short window when it sends the autotune;
// a key edge inside the window is a *provisional* tune that the tuner's
// tuning=1 must confirm within kTgxlTuneConfirmMs, otherwise the tuner refused
// and the over belongs to the operator. Clock-injected so it is unit-testable.
inline constexpr qint64 kTgxlAutotuneKeyWindowMs = 5000;
inline constexpr qint64 kTgxlTuneConfirmMs = 1500;

struct TgxlTuneCycle {
    qint64 armedUntilMs{0};
    bool   provisional{false};
    qint64 confirmByMs{0};
    bool   latched{false};
    // tuning was still true when the cycle unkeyed; ignore it until tuning=0
    // so the late completion can't latch the operator's next over.
    bool   staleTuning{false};
    bool   wasTransmitting{false};

    void arm(qint64 nowMs) { armedUntilMs = nowMs + kTgxlAutotuneKeyWindowMs; }

    void reset() { *this = TgxlTuneCycle{}; }

    void onTuningChanged(bool tuning, bool transmitting)
    {
        if (tuning)
            return;
        staleTuning = false;
        if (!transmitting)
            armedUntilMs = 0;  // tune ended/refused unkeyed: a quick MOX is the operator's
    }

    // Returns true while this cycle is a (possible) TGXL tune carrier.
    bool update(bool transmitting, bool tunerTuning, qint64 nowMs)
    {
        if (!transmitting) {
            if (latched || provisional) {
                armedUntilMs = 0;
                staleTuning = tunerTuning;
            }
            latched = false;
            provisional = false;
            wasTransmitting = false;
            return false;
        }

        const bool liveTuning = tunerTuning && !staleTuning;
        const bool rising = !wasTransmitting;
        wasTransmitting = true;

        if (rising) {
            if (liveTuning) {
                latched = true;
                provisional = false;
            } else if (nowMs < armedUntilMs) {
                provisional = true;
                confirmByMs = nowMs + kTgxlTuneConfirmMs;
            }
        } else if (provisional) {
            if (liveTuning) {
                latched = true;
                provisional = false;
            } else if (nowMs >= confirmByMs) {
                provisional = false;
                armedUntilMs = 0;
            }
        } else if (!latched && liveTuning) {
            latched = true;  // front-panel tune: no arm, tuning=1 mid-over
        }
        return latched || provisional;
    }
};

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

} // namespace AetherSDR::RadioStatusOwnership
