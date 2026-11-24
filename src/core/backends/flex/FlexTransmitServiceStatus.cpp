#include "FlexTransmitServiceStatus.h"

namespace AetherSDR {

std::optional<DvkStatusObservation> decodeFlexDvkStatus(const QString& object, const QMap<QString, QString>& kvs)
{
    if (!object.startsWith(QLatin1String("dvk"))) { return std::nullopt; }
    DvkStatusObservation out;
    // Global status: "dvk status=idle enabled=1 [id=N]".
    if (kvs.contains(QStringLiteral("status"))) {
        using State = DvkStatusObservation::State;
        const QString s = kvs.value(QStringLiteral("status"));
        out.state = s == QLatin1String("idle") ? State::Idle
            : s == QLatin1String("recording") ? State::Recording
            : s == QLatin1String("preview") ? State::Preview
            : s == QLatin1String("playback") ? State::Playback
            : s == QLatin1String("disabled") ? State::Disabled : State::Unknown;
        if (kvs.contains(QStringLiteral("id"))) { out.id = kvs.value(QStringLiteral("id")).toInt(); }
        if (kvs.contains(QStringLiteral("enabled"))) { out.enabled = kvs.value(QStringLiteral("enabled")) == QLatin1String("1"); }
        // FlexLib DVKStatus: enabled=0 means disabled, and the id is void.
        if (kvs.value(QStringLiteral("enabled")) == QLatin1String("0")) {
            out.state = State::Disabled;
            out.id = -1;
        }
        return out;
    }
    const int id = kvs.value(QStringLiteral("id")).toInt();
    if (id <= 0) { return std::nullopt; }
    out.id = id;
    // "dvk deleted id=1" or "dvk id=1 deleted".
    if (object.contains(QLatin1String("deleted"))) {
        out.kind = DvkStatusObservation::Kind::Deleted;
        return out;
    }
    // Added or updated: id=N name="..." duration=NNNN. The name is quoted and
    // may hold spaces; the parser keeps a quoted value whole, quotes included.
    out.kind = DvkStatusObservation::Kind::Recording;
    out.name = kvs.value(QStringLiteral("name"));
    out.name.remove(QLatin1Char('"'));
    out.durationMs = kvs.value(QStringLiteral("duration"), QStringLiteral("0")).toInt();
    return out;
}

std::optional<NavtexStatusObservation> decodeFlexNavtexStatus(const QString& object, const QMap<QString, QString>& kvs)
{
    NavtexStatusObservation out;
    // "navtex sent idx=N serial_num=M": one message's transmit confirmation.
    if (object == QLatin1String("navtex sent")) {
        bool ok = false;
        out.idx = kvs.value(QStringLiteral("idx")).toUInt(&ok);
        if (!ok) { return std::nullopt; }
        if (kvs.contains(QStringLiteral("serial_num"))) {
            out.serial = kvs.value(QStringLiteral("serial_num")).toUInt(&ok);
            if (!ok) { return std::nullopt; }
        }
        out.kind = NavtexStatusObservation::Kind::Sent;
        return out;
    }
    if (!object.startsWith(QLatin1String("navtex")) || !kvs.contains(QStringLiteral("status"))) { return std::nullopt; }
    // Case-insensitive, as FlexLib's Enum.TryParse reads it.
    const QString lower = kvs.value(QStringLiteral("status")).toLower();
    out.state = lower == QLatin1String("inactive") ? NavtexState::Inactive
        : lower == QLatin1String("active") ? NavtexState::Active
        : lower == QLatin1String("transmitting") ? NavtexState::Transmitting
        : lower == QLatin1String("queuefull") ? NavtexState::QueueFull
        : lower == QLatin1String("unlicensed") ? NavtexState::Unlicensed : NavtexState::Error;
    return out;
}

} // namespace AetherSDR
