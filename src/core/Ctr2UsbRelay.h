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
// radio TCP connection. While no link is up the host calls the device with
// HELLO; the device's READY (or its own HELLO, a restart request) gets a fresh
// radio connection, and READY and CLOSED tell the device when that connection
// opens and ends. Link faults close the radio connection and send CLOSED;
// nothing is ever injected into either byte stream. Devices that negotiate
// link extensions (Ctr2HidPort::extensions) can also be sent AetherSDR's own
// messages beside the radio stream, such as the audio spectrum.
// Specification: docs/ctr2-usb-relay-design.md ("USB link specification").
class Ctr2UsbRelay : public QObject {
    Q_OBJECT

public:
    // Listening means "calling the CTR2 and waiting for its answer" in USB mode.
    using State = TcpByteProxy::State;
    using Stats = TcpByteProxy::Stats;

    struct Tuning {
        int connectTimeoutMs{10000};
        int incompleteMessageTimeoutMs{1000};
        // While no link is up the host calls the CTR2 with HELLO this often.
        // After a failure (the radio connection failed, or a link fault such
        // as a framing error) it calls at the slower rate until a link ends
        // cleanly or the operator restarts.
        int helloIntervalMs{1000};
        int helloRetryAfterFaultMs{5000};
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

    // Link extensions the open device negotiated (ctr2hid::capabilityBit mask).
    quint32 extensions() const;
    // Sends one AudioSpectrum message while relaying, if the device took that
    // extension. Returns false, sending nothing, otherwise or when ~130 ms of
    // output is already queued: a late frame is worth less than a fresh one,
    // and it must not delay the radio stream.
    bool sendAudioSpectrum(const QByteArray& payload);

signals:
    void stateChanged(AetherSDR::TcpByteProxy::State state);
    void statsChanged();
    void endpointsChanged();
    void lastErrorChanged(const QString& message);
    // The device's negotiated link extensions arrived; extensions() changed.
    void extensionsChanged(quint32 extensions);

private:
    class Session;
    class DeviceEndpoint;
    friend class Session;
    friend class DeviceEndpoint;

    void onReportsReceived(const QByteArray& reports);
    void onReportsSent(int count);
    void onPortFailed(const QString& message);
    void onExtensionsNegotiated(quint32 extensions);
    void onMessage(const ctr2hid::Message& message);
    void onDeviceStart();
    void sendHello();
    void scheduleHello(int delayMs);
    void linkFault(const QString& message);

    void sendControl(ctr2hid::MessageType type);
    void sendData(const QByteArray& payload);
    bool sendDatagram(quint16 port, const QByteArray& datagram);
    void sessionConnected(quint64 generation, quint16 udpPort);
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
    QTimer* m_helloTimer{nullptr};
    bool m_linkDown{true};  // no link up: calling the CTR2, or draining the last one
    int m_helloDelayMs{1000};  // current calling interval; slower after a failure
    bool m_closedSent{false};  // CLOSED already queued for this link
    // A HELLO has gone out since the last link ended, so a device READY is an
    // answer to it. False during failure back-off and while CLOSED is pending.
    bool m_helloSent{false};
    static constexpr int kMaxQueuedReports = 8192;
    quint64 m_generation{0};
    State m_state{State::Stopped};
    QString m_lastError;
    Stats m_lastSessionStats;
};

} // namespace AetherSDR
