#pragma once

#include <functional>
#include <concepts>
#include <optional>
#include <type_traits>
#include <utility>
#include "SessionCommandGuard.h"
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
class RadioControlFence {
public:
    RadioControlFence() = default;
    template <typename Predicate>
        requires (!std::same_as<std::remove_cvref_t<Predicate>, RadioControlFence>
                  && std::is_invocable_r_v<bool, Predicate>)
    RadioControlFence(Predicate&& predicate) : m_current(std::forward<Predicate>(predicate)) {}
    explicit operator bool() const { return bool(m_current); }
    bool operator()() const { return m_current && m_current(); }
    RadioControlFence withWriteGuard(const SessionCommandGuard& guard) const
    {
        RadioControlFence fence(*this);
        fence.m_guard = guard;
        return fence;
    }
    const std::optional<SessionCommandGuard>& writeGuard() const { return m_guard; }
    RadioControlFence forCleanup() const
    {
        RadioControlFence fence(*this);
        fence.m_cleanup = true;
        return fence;
    }
    bool cleanup() const { return m_cleanup; }
private:
    // Owner predicates stay on the owner thread unless the accepting service
    // explicitly requires an immutable, worker-safe predicate. Administrative
    // transport writes evaluate the separate value guard.
    std::function<bool()> m_current;
    std::optional<SessionCommandGuard> m_guard;
    bool m_cleanup{false};
};
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
