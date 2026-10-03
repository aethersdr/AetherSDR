#pragma once

#include "ByteRelay.h"
#include "Ctr2HidFraming.h"
#include "TcpByteProxy.h"

#include <QHostAddress>
#include <QObject>
#include <QString>

#include <deque>

class QTimer;

namespace AetherSDR {

class Ctr2HidPort;

// CTR2 USB mode: one HID link (Ctr2HidPort) relayed opaquely to a dedicated
// radio TCP connection. Every HELLO from the device gets a fresh radio
// connection; READY and CLOSED tell the device when the radio connection
// opens and ends. Link faults close the radio connection and send CLOSED;
// nothing is ever injected into either byte stream.
// Specification: docs/ctr2-usb-relay-design.md ("USB link specification").
class Ctr2UsbRelay : public QObject {
    Q_OBJECT

public:
    // Listening means "waiting for HELLO" in USB mode.
    using State = TcpByteProxy::State;
    using Stats = TcpByteProxy::Stats;

    struct Tuning {
        int connectTimeoutMs{10000};
        int incompleteMessageTimeoutMs{1000};
        qint64 deviceInboxLimitBytes{256 * 1024};
        qint64 socketReadBufferBytes{256 * 1024};
        // UDP waiting for the HID link; beyond this, radio datagrams are
        // dropped rather than delaying the TCP stream.
        qint64 datagramBacklogBytes{3 * 1024};
        // USB moves ~7 KB/s each way, so the per-direction budget is small
        // enough to drain well inside the relay's drain timeout.
        ByteRelay::Limits relay{16 * 1024};
    };

    explicit Ctr2UsbRelay(QObject* parent = nullptr);
    ~Ctr2UsbRelay() override;

    void setTuning(const Tuning& tuning) { m_tuning = tuning; }
    void setRadioSocketFactory(TcpByteProxy::SocketFactory factory);

    // Takes ownership of an opened port. The radio endpoint is fixed until stop().
    bool start(Ctr2HidPort* port, const QHostAddress& radioAddress, quint16 radioPort);
    void stop();

    State state() const { return m_state; }
    QString lastError() const { return m_lastError; }
    QString deviceDescription() const;
    QString radioDescription() const;
    Stats stats() const;
    quint64 linkGeneration() const { return m_generation; }

signals:
    void stateChanged(AetherSDR::TcpByteProxy::State state);
    void statsChanged();
    void endpointsChanged();
    void lastErrorChanged(const QString& message);

private:
    class Session;
    class DeviceEndpoint;
    friend class Session;
    friend class DeviceEndpoint;

    void onReportsReceived(const QByteArray& reports);
    void onReportsSent(int count);
    void onPortFailed(const QString& message);
    void onMessage(const ctr2hid::Message& message);
    void onHello();
    void linkFault(const QString& message);

    void sendControl(ctr2hid::MessageType type);
    void sendData(const QByteArray& payload);
    bool sendDatagram(quint16 port, const QByteArray& datagram);
    void sessionConnected(quint64 generation);
    void sessionDraining(quint64 generation);
    void sessionEnded(quint64 generation, const QString& message, bool error, bool sendClosed);
    void endSession();
    void releasePort(const std::vector<ctr2hid::Report>& finalReports);
    void discardOutput();
    void sendClosedOnce();
    void setState(State state);
    void setLastError(const QString& message);

    Tuning m_tuning;
    TcpByteProxy::SocketFactory m_socketFactory;
    Ctr2HidPort* m_port{nullptr};
    QHostAddress m_radioAddress;
    quint16 m_radioPort{0};
    ctr2hid::FrameReassembler m_rx;
    ctr2hid::FrameEncoder m_tx;
    struct ReportCost {
        int tcpBytes{0};
        int datagramBytes{0};
    };
    std::deque<ReportCost> m_reportCosts;  // one per report queued on the port
    qint64 m_unsentPayload{0};             // TCP bytes queued on the port
    qint64 m_unsentDatagramBytes{0};
    Session* m_session{nullptr};
    QTimer* m_incompleteTimer{nullptr};
    bool m_awaitingHello{true};
    bool m_closedSent{false};  // CLOSED already queued for this link
    static constexpr int kMaxQueuedReports = 8192;
    quint64 m_generation{0};
    State m_state{State::Stopped};
    QString m_lastError;
    Stats m_lastSessionStats;
};

} // namespace AetherSDR
