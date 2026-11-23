#pragma once

// The byte transport and clock a MicrohamTrioSession runs on outside tests: a
// QTcpSocket per connection and a monotonic timer. Main-thread only.

#include "core/MicrohamTrioSession.h"

#include <QElapsedTimer>

class QTcpSocket;

namespace AetherSDR {

class MicrohamTrioTcpTransport final : public MicrohamTrioTransport {
    Q_OBJECT

public:
    explicit MicrohamTrioTcpTransport(QObject* parent = nullptr);
    ~MicrohamTrioTcpTransport() override;

    void open(const QString& host, quint16 port) override;
    void send(const QByteArray& bytes) override;
    void close() override;

private:
    QTcpSocket* m_socket{nullptr};
};

class MicrohamTrioMonotonicClock final : public MicrohamTrioClock {
public:
    MicrohamTrioMonotonicClock() { m_timer.start(); }
    qint64 nowMs() const override { return m_timer.elapsed(); }

private:
    QElapsedTimer m_timer;
};

} // namespace AetherSDR
