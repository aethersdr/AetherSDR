#pragma once

#include <QObject>
#include <QTimer>

#include <functional>

namespace AetherSDR {

// One extension-set notification, coalesced and rate-limited like drive:
// (TciServer::queuePowerBroadcast): queue() posts one send, so a burst of
// changes in one event-loop turn goes out once; after a send that sent
// something, further changes wait for the interval and go out as one trailing
// send. `send` reads the current state and returns whether it sent a line.
class TciExtensionNotice {
public:
    TciExtensionNotice(QObject* context, int intervalMs, std::function<bool()> send);

    void queue();

private:
    void flush();

    QObject* m_context;
    QTimer m_timer;
    std::function<bool()> m_send;
    bool m_pending{false};
    bool m_posted{false};
};

} // namespace AetherSDR
