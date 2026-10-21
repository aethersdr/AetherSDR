#pragma once

#include <functional>
#include <QMetaType>
#include <QPointer>
#include <QObject>
#include <QString>

namespace AetherSDR {

// A command reply is not an observation of radio state or durable storage.
struct RadioControlReply {
    enum class Kind { Accepted, Refused, Failed, Cancelled };
    Kind kind;
    int code{0};
    QString message;
};
using RadioControlCompletion = std::function<void(const RadioControlReply&)>;
// Trusted composition fences writes to their original session, not TX authority.
using RadioControlFence = std::function<bool()>;
struct RadioControlSession {
    quint64 backendGeneration;
    quint64 connectionGeneration;
    bool operator==(const RadioControlSession&) const = default;
};
struct RadioStationOverride {
    QPointer<QObject> model;
    RadioControlSession session;
    quint64 revision;
};

} // namespace AetherSDR

Q_DECLARE_METATYPE(AetherSDR::RadioControlReply)
