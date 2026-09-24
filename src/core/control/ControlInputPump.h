#pragma once

#include "ControlService.h"

#include <QPointer>

#include <functional>

namespace AetherSDR::control {

// Production input framing/scheduling with an injected byte transport. Bounded
// work per event-loop turn lets engine deadline timers run during client bursts.
// This owns neither authentication nor a TX arbiter. No socket is needed to
// exercise its terminal lifetime and scheduler behavior.
// Trusted composition supplies nonempty callbacks and a service that outlives
// the pump. Pump, session and transport stay on this owning thread throughout.
class ControlInputPump final : public QObject {
public:
    static constexpr int kFramesPerTurn = 16;
    static constexpr qint64 kReadBytesPerTurn = 64 * 1024;
    using Read = std::function<QByteArray(qint64 maximumBytes)>;
    using Write = std::function<bool(const QJsonObject&)>;
    using Close = std::function<void(bool abort)>;

    ControlInputPump(ControlService& service, ControlSession& session,
                     Read read, Write write, Close close, QObject* parent = nullptr);
    ~ControlInputPump() override;
    void readAvailable();
    // Called synchronously on every terminal transport edge. Qt object
    // destruction may remain deferred without extending authority lifetime.
    void finish();
    [[nodiscard]] bool isFinished() const { return m_finished; }

private:
    void schedule();
    [[nodiscard]] bool deliver(const ServiceReply& reply);
    void oversizedFrame();

    ControlService& m_service;
    QPointer<ControlSession> m_session;
    Read m_read;
    Write m_write;
    Close m_close;
    QByteArray m_input;
    bool m_reading{false};
    bool m_scheduled{false};
    bool m_finished{false};
};

} // namespace AetherSDR::control
