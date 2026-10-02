#include "Ctr2UsbRelay.h"

#include "Ctr2HidPort.h"
#include "LogManager.h"
#include "TcpSocketEndpoint.h"

#include <QAbstractSocket>
#include <QNetworkDatagram>
#include <QTcpSocket>
#include <QUdpSocket>
#include <QTimer>

#include <algorithm>
#include <utility>

namespace AetherSDR {

using ctr2hid::Message;
using ctr2hid::MessageType;

namespace {

QString endpointText(const QHostAddress& address, quint16 port)
{
    return QStringLiteral("%1:%2").arg(address.toString()).arg(port);
}

bool usableIpv4(const QHostAddress& address)
{
    bool ok = false;
    const quint32 v4 = address.toIPv4Address(&ok);
    return ok && v4 != 0 && v4 != 0xFFFFFFFFu && !address.isMulticast();
}

} // namespace

// The device side of the relay: reassembled DATA payload in, payload out as
// encoded reports on the port. bytesToWrite() is payload still queued there.
class Ctr2UsbRelay::DeviceEndpoint final : public ByteRelayEndpoint {
public:
    DeviceEndpoint(Ctr2UsbRelay* owner, Session* session) : m_owner(owner), m_session(session) {}

    qint64 bytesAvailable() const override { return m_inbox.size(); }
    QByteArray read(qint64 maxBytes) override
    {
        const QByteArray out = m_inbox.left(maxBytes);
        m_inbox.remove(0, out.size());
        return out;
    }
    qint64 write(const QByteArray& data) override
    {
        if (!m_owner->m_port || m_closeRequested) {
            return -1;
        }
        m_owner->sendData(data);
        return data.size();
    }
    qint64 bytesToWrite() const override { return m_owner->m_unsentPayload; }
    void closeGracefully() override;
    void abort() override { m_inbox.clear(); }

    void append(const QByteArray& payload) { m_inbox.append(payload); }

    bool deviceClosed{false};

private:
    Ctr2UsbRelay* m_owner;
    Session* m_session;
    QByteArray m_inbox;
    bool m_closeRequested{false};
};

// One HELLO's worth of link: a fresh radio connection plus the relay over it.
class Ctr2UsbRelay::Session : public QObject {
public:
    Session(Ctr2UsbRelay* owner, quint64 generation, QTcpSocket* radio)
        : QObject(owner)
        , m_owner(owner)
        , m_generation(generation)
        , m_radio(radio)
        , m_radioEp(radio)
        , m_deviceEp(owner, this)
    {
        m_radio->setParent(this);
        m_radio->setReadBufferSize(owner->m_tuning.socketReadBufferBytes);
        connect(m_radio, &QAbstractSocket::connected, this, [this] { onConnected(); });
        connect(m_radio, &QAbstractSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
            if (!m_relay) {
                end(QStringLiteral("Radio connection failed: %1").arg(m_radio->errorString()),
                    true, true);
            }
        });
        m_connectTimer = new QTimer(this);
        m_connectTimer->setSingleShot(true);
        m_connectTimer->setInterval(std::max(1, owner->m_tuning.connectTimeoutMs));
        connect(m_connectTimer, &QTimer::timeout, this, [this] {
            end(QStringLiteral("Radio connection timed out after %1 ms")
                    .arg(m_owner->m_tuning.connectTimeoutMs),
                true, true);
        });
    }

    ~Session() override
    {
        QObject::disconnect(m_radio, nullptr, this, nullptr);
        if (m_udp) {
            QObject::disconnect(m_udp, nullptr, this, nullptr);
            m_udp->close();
        }
        m_radio->abort();
        delete m_relay;
        m_relay = nullptr;
    }

    void begin(const QHostAddress& address, quint16 port)
    {
        m_connectTimer->start();
        m_radio->connectToHost(address.toString(), port);
    }

    bool relaying() const { return m_relay != nullptr && !m_ended; }

    void deviceData(const QByteArray& payload)
    {
        m_deviceEp.append(payload);
        if (m_deviceEp.bytesAvailable() > m_owner->m_tuning.deviceInboxLimitBytes) {
            m_owner->linkFault(QStringLiteral("CTR2 sent more than the relay can buffer"));
            return;
        }
        m_relay->notifyReadable(ByteRelay::Side::A);
    }

    void deviceDatagram(quint16 port, const QByteArray& datagram)
    {
        if (!m_udp) {
            return;
        }
        if (m_udp->writeDatagram(datagram, m_owner->m_radioAddress, port) == datagram.size()) {
            ++m_datagramsToRadio;
        }
    }

    void deviceClosed()
    {
        m_deviceEp.deviceClosed = true;
        if (m_relay) {
            m_relay->notifyEndOfInput(ByteRelay::Side::A);
        } else {
            end(QStringLiteral("CTR2 closed the link before the radio connected"), false, false);
        }
    }

    void deviceBytesWritten()
    {
        if (m_relay && !m_ended) {
            m_relay->notifyBytesWritten(ByteRelay::Side::A);
        }
    }

    void deviceCloseSent()
    {
        if (m_relay && !m_ended) {
            m_relay->notifyClosed(ByteRelay::Side::A);
        }
    }

    void terminate(const QString& message)
    {
        if (m_ended) {
            return;
        }
        if (m_relay) {
            m_stopMessage = message;
            m_relay->stop();
            return;
        }
        end(message, false, false);
    }

    Stats stats() const
    {
        Stats s;
        if (m_relay) {
            const ByteRelay::Stats r = m_relay->stats();
            s.toUpstream = r.forwarded[0];
            s.toDownstream = r.forwarded[1];
            s.queuedToUpstream = r.queued[0];
            s.queuedToDownstream = r.queued[1];
        }
        s.datagramsToRadio = m_datagramsToRadio;
        s.datagramsToDevice = m_datagramsToDevice;
        s.datagramsDropped = m_datagramsDropped;
        return s;
    }

    Stats m_finalStats;

private:
    void onConnected()
    {
        if (m_ended || m_relay) {
            return;
        }
        m_connectTimer->stop();
        m_radio->setSocketOption(QAbstractSocket::LowDelayOption, 1);
        m_relay = new ByteRelay(&m_deviceEp, &m_radioEp, m_owner->m_tuning.relay);
        connect(m_radio, &QIODevice::readyRead, this, [this] {
            if (m_relay) {
                m_relay->notifyReadable(ByteRelay::Side::B);
            }
        });
        connect(m_radio, &QIODevice::bytesWritten, this, [this](qint64) {
            if (m_relay) {
                m_relay->notifyBytesWritten(ByteRelay::Side::B);
            }
        });
        connect(m_radio, &QIODevice::readChannelFinished, this, [this] {
            if (m_relay) {
                m_relay->notifyEndOfInput(ByteRelay::Side::B);
            }
        });
        connect(m_radio, &QAbstractSocket::disconnected, this, [this] {
            if (m_relay) {
                m_relay->notifyClosed(ByteRelay::Side::B);
            }
        });
        connect(m_radio, &QAbstractSocket::errorOccurred, this,
                [this](QAbstractSocket::SocketError err) {
            if (!m_relay) {
                return;
            }
            if (err == QAbstractSocket::RemoteHostClosedError) {
                m_relay->notifyEndOfInput(ByteRelay::Side::B);
                return;
            }
            m_relay->notifyError(ByteRelay::Side::B, m_radio->errorString());
        });
        connect(m_relay, &ByteRelay::statsChanged, m_owner, &Ctr2UsbRelay::statsChanged);
        connect(m_relay, &ByteRelay::drainStarted, this, [this] {
            m_owner->sessionDraining(m_generation);
        });
        connect(m_relay, &ByteRelay::finished, this,
                [this](ByteRelay::EndReason reason, const QString& message,
                       qint64 discardedA, qint64 discardedB) {
            QString text;
            switch (reason) {
            case ByteRelay::EndReason::PeerClosed:
                text = m_deviceEp.deviceClosed ? QStringLiteral("CTR2 closed the link")
                                               : QStringLiteral("Radio closed the connection");
                break;
            case ByteRelay::EndReason::Stopped:
                text = m_stopMessage.isEmpty() ? QStringLiteral("Stopped") : m_stopMessage;
                break;
            default:
                text = message;
                break;
            }
            text += QStringLiteral(" (discarded %1 bytes toward radio, %2 toward CTR2)")
                        .arg(discardedA).arg(discardedB);
            const bool error = reason != ByteRelay::EndReason::PeerClosed
                && reason != ByteRelay::EndReason::Stopped;
            // A graceful radio close already sent CLOSED after the last byte.
            end(text, error, error);
        });

        // One UDP socket per link: the radio learns this endpoint from the
        // CTR2's own registration datagram, so its UDP never reaches
        // AetherSDR's sockets and no port needs negotiating.
        m_udp = new QUdpSocket(this);
        if (!m_udp->bind(QHostAddress::AnyIPv4, 0)) {
            qCWarning(lcDevices) << "CTR2 USB: UDP socket unavailable:" << m_udp->errorString();
            delete m_udp;
            m_udp = nullptr;
        } else {
            connect(m_udp, &QUdpSocket::readyRead, this, [this] { onRadioDatagrams(); });
        }

        m_owner->sessionConnected(m_generation);
        if (!m_ended) {
            m_relay->start();
        }
    }

    void onRadioDatagrams()
    {
        const quint32 radio = m_owner->m_radioAddress.toIPv4Address();
        while (m_udp && m_udp->hasPendingDatagrams()) {
            const QNetworkDatagram d = m_udp->receiveDatagram(ctr2hid::kMaxDatagramBytes + 1);
            bool ok = false;
            if (d.senderAddress().toIPv4Address(&ok) != radio || !ok || m_ended) {
                continue;  // only the radio's own datagrams are relayed
            }
            const QByteArray bytes = d.data();
            if (bytes.isEmpty() || bytes.size() > ctr2hid::kMaxDatagramBytes
                || m_owner->m_unsentDatagramBytes + bytes.size()
                       > m_owner->m_tuning.datagramBacklogBytes
                || !m_owner->sendDatagram(static_cast<quint16>(d.senderPort()), bytes)) {
                ++m_datagramsDropped;
                continue;
            }
            ++m_datagramsToDevice;
        }
        emit m_owner->statsChanged();
    }

    void end(const QString& message, bool error, bool sendClosed)
    {
        if (m_ended) {
            return;
        }
        m_ended = true;
        m_connectTimer->stop();
        m_finalStats = stats();
        if (!m_relay) {
            m_radio->abort();
        }
        m_owner->sessionEnded(m_generation, message, error, sendClosed);
    }

    Ctr2UsbRelay* m_owner;
    quint64 m_generation;
    QTcpSocket* m_radio;
    TcpSocketEndpoint m_radioEp;
    DeviceEndpoint m_deviceEp;
    ByteRelay* m_relay{nullptr};
    QTimer* m_connectTimer{nullptr};
    QUdpSocket* m_udp{nullptr};
    quint64 m_datagramsToRadio{0};
    quint64 m_datagramsToDevice{0};
    quint64 m_datagramsDropped{0};
    QString m_stopMessage;
    bool m_ended{false};

    friend class DeviceEndpoint;
};

void Ctr2UsbRelay::DeviceEndpoint::closeGracefully()
{
    if (deviceClosed || m_closeRequested) {
        return;  // the device ended the link; it expects no reply
    }
    // The port is FIFO, so CLOSED follows every radio byte already queued.
    m_closeRequested = true;
    m_owner->sendControl(MessageType::Closed);
    m_session->deviceCloseSent();
}

Ctr2UsbRelay::Ctr2UsbRelay(QObject* parent)
    : QObject(parent)
    , m_socketFactory([] { return new QTcpSocket; })
{
    m_incompleteTimer = new QTimer(this);
    m_incompleteTimer->setSingleShot(true);
    connect(m_incompleteTimer, &QTimer::timeout, this, [this] {
        linkFault(QStringLiteral("CTR2 message incomplete after %1 ms")
                      .arg(m_tuning.incompleteMessageTimeoutMs));
    });
}

Ctr2UsbRelay::~Ctr2UsbRelay()
{
    stop();
}

void Ctr2UsbRelay::setRadioSocketFactory(TcpByteProxy::SocketFactory factory)
{
    m_socketFactory = factory ? std::move(factory)
                              : TcpByteProxy::SocketFactory([] { return new QTcpSocket; });
}

bool Ctr2UsbRelay::start(Ctr2HidPort* port, const QHostAddress& radioAddress, quint16 radioPort)
{
    if (m_state != State::Stopped && m_state != State::Error) {
        return false;
    }
    if (!port || !port->isOpen()) {
        setLastError(QStringLiteral("No CTR2 USB device is open"));
        setState(State::Error);
        return false;
    }
    if (!usableIpv4(radioAddress) || radioPort == 0) {
        setLastError(QStringLiteral("Enter the radio's IPv4 address and port"));
        setState(State::Error);
        return false;
    }
    m_port = port;
    m_port->setParent(this);
    connect(m_port, &Ctr2HidPort::reportsReceived, this, &Ctr2UsbRelay::onReportsReceived);
    connect(m_port, &Ctr2HidPort::reportsSent, this, &Ctr2UsbRelay::onReportsSent);
    connect(m_port, &Ctr2HidPort::failed, this, &Ctr2UsbRelay::onPortFailed);
    m_radioAddress = QHostAddress(radioAddress.toIPv4Address());
    m_radioPort = radioPort;
    m_rx.reset();
    m_tx.reset();
    m_reportCosts.clear();
    m_unsentPayload = 0;
    m_unsentDatagramBytes = 0;
    m_awaitingHello = true;
    m_lastSessionStats = {};
    ++m_generation;
    setLastError({});
    qCInfo(lcDevices) << "CTR2 USB: waiting for HELLO from" << m_port->description()
                      << "-> radio" << radioDescription();
    setState(State::Listening);
    emit endpointsChanged();
    emit statsChanged();
    return true;
}

void Ctr2UsbRelay::stop()
{
    if (m_session) {
        m_session->terminate(QStringLiteral("Stopped"));
    }
    endSession();
    m_incompleteTimer->stop();
    if (m_port) {
        if (m_port->isOpen()) {
            // Queued radio bytes are dropped; CLOSED still reaches the device.
            std::vector<ctr2hid::Report> closed;
            m_tx.encodeControl(MessageType::Closed, &closed);
            m_port->closeAfter(closed);
        }
        releasePort();
        qCInfo(lcDevices) << "CTR2 USB: stopped";
    }
    ++m_generation;
    setState(State::Stopped);
    emit endpointsChanged();
    emit statsChanged();
}

void Ctr2UsbRelay::releasePort()
{
    if (!m_port) {
        return;
    }
    Ctr2HidPort* port = m_port;
    m_port = nullptr;
    QObject::disconnect(port, nullptr, this, nullptr);
    port->close();
    port->deleteLater();
    m_reportCosts.clear();
    m_unsentPayload = 0;
    m_unsentDatagramBytes = 0;
}

void Ctr2UsbRelay::onReportsReceived(const QByteArray& reports)
{
    for (qsizetype off = 0; off + ctr2hid::kReportBytes <= reports.size();
         off += ctr2hid::kReportBytes) {
        if (!m_port) {
            return;
        }
        std::vector<Message> messages;
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(reports.constData() + off);
        if (!m_rx.feed(bytes, ctr2hid::kReportBytes, &messages)) {
            const QString err = m_rx.errorText();
            m_rx.reset();
            if (!m_awaitingHello) {
                linkFault(QStringLiteral("CTR2 link framing error: %1").arg(err));
            }
            continue;
        }
        for (const Message& m : messages) {
            onMessage(m);
            if (!m_port) {
                return;
            }
        }
    }
    if (m_rx.isMidMessage()) {
        if (!m_incompleteTimer->isActive()) {
            m_incompleteTimer->start(std::max(1, m_tuning.incompleteMessageTimeoutMs));
        }
    } else {
        m_incompleteTimer->stop();
    }
}

void Ctr2UsbRelay::onMessage(const Message& message)
{
    switch (message.type) {
    case MessageType::Hello:
        onHello();
        return;
    case MessageType::Data:
        if (m_awaitingHello) {
            // In flight before the device saw our CLOSED; it restarts with HELLO.
            return;
        }
        if (!m_session || !m_session->relaying()) {
            linkFault(QStringLiteral("CTR2 sent data before READY"));
            return;
        }
        m_session->deviceData(message.payload);
        return;
    case MessageType::Datagram:
        if (m_awaitingHello) {
            return;
        }
        if (!m_session || !m_session->relaying()) {
            linkFault(QStringLiteral("CTR2 sent a datagram before READY"));
            return;
        }
        m_session->deviceDatagram(message.port, message.payload);
        return;
    case MessageType::Closed:
        m_awaitingHello = true;
        if (m_session) {
            m_session->deviceClosed();
        }
        return;
    case MessageType::Ready:
        linkFault(QStringLiteral("CTR2 sent READY, which only the host sends"));
        return;
    }
}

void Ctr2UsbRelay::onHello()
{
    if (m_session) {
        m_session->terminate(QStringLiteral("CTR2 restarted the link"));
    }
    endSession();
    m_awaitingHello = false;
    ++m_generation;
    m_lastSessionStats = {};
    QTcpSocket* radio = m_socketFactory();
    m_session = new Session(this, m_generation, radio);
    qCInfo(lcDevices) << "CTR2 USB: HELLO; opening" << radioDescription();
    setState(State::Connecting);
    emit endpointsChanged();
    m_session->begin(m_radioAddress, m_radioPort);
}

void Ctr2UsbRelay::linkFault(const QString& message)
{
    if (m_session) {
        m_session->terminate(message);
    }
    endSession();
    m_incompleteTimer->stop();
    m_rx.reset();
    m_awaitingHello = true;
    qCWarning(lcDevices) << "CTR2 USB:" << message;
    setLastError(message);
    if (m_port) {
        sendControl(MessageType::Closed);
        setState(State::Listening);
    }
    emit endpointsChanged();
    emit statsChanged();
}

void Ctr2UsbRelay::onReportsSent(int count)
{
    qint64 tcp = 0;
    qint64 datagram = 0;
    for (int i = 0; i < count && !m_reportCosts.empty(); ++i) {
        tcp += m_reportCosts.front().tcpBytes;
        datagram += m_reportCosts.front().datagramBytes;
        m_reportCosts.pop_front();
    }
    m_unsentPayload = std::max<qint64>(0, m_unsentPayload - tcp);
    m_unsentDatagramBytes = std::max<qint64>(0, m_unsentDatagramBytes - datagram);
    if (m_session) {
        m_session->deviceBytesWritten();
    }
}

void Ctr2UsbRelay::onPortFailed(const QString& message)
{
    if (m_session) {
        m_session->terminate(message);
    }
    endSession();
    m_incompleteTimer->stop();
    releasePort();
    setLastError(message);
    setState(State::Error);
    emit endpointsChanged();
    emit statsChanged();
}

void Ctr2UsbRelay::sendControl(MessageType type)
{
    if (!m_port) {
        return;
    }
    std::vector<ctr2hid::Report> reports;
    m_tx.encodeControl(type, &reports);
    m_reportCosts.push_back({});
    m_port->send(reports);
}

void Ctr2UsbRelay::sendData(const QByteArray& payload)
{
    if (!m_port || payload.isEmpty()) {
        return;
    }
    std::vector<ctr2hid::Report> reports;
    m_tx.encodeData(payload, &reports);
    // Header reports carry no payload; data reports carry up to 7 bytes.
    int remainingInMessage = 0;
    qsizetype consumed = 0;
    for (const ctr2hid::Report& r : reports) {
        if (remainingInMessage == 0) {
            remainingInMessage = (r[6] << 8) | r[7];
            m_reportCosts.push_back({});
            continue;
        }
        const int n = std::min(ctr2hid::kDataBytesPerReport, remainingInMessage);
        remainingInMessage -= n;
        consumed += n;
        m_reportCosts.push_back({n, 0});
    }
    m_unsentPayload += consumed;
    m_port->send(reports);
}

bool Ctr2UsbRelay::sendDatagram(quint16 port, const QByteArray& datagram)
{
    if (!m_port) {
        return false;
    }
    std::vector<ctr2hid::Report> reports;
    if (!m_tx.encodeDatagram(port, datagram, &reports)) {
        return false;
    }
    // The datagram's cost is released when its last report is sent.
    for (size_t i = 0; i + 1 < reports.size(); ++i) {
        m_reportCosts.push_back({});
    }
    m_reportCosts.push_back({0, static_cast<int>(datagram.size())});
    m_unsentDatagramBytes += datagram.size();
    m_port->send(reports);
    return true;
}

void Ctr2UsbRelay::sessionConnected(quint64 generation)
{
    if (generation != m_generation) {
        return;
    }
    m_tx.reset();
    sendControl(MessageType::Ready);
    qCInfo(lcDevices) << "CTR2 USB: READY; relaying" << deviceDescription()
                      << "<->" << radioDescription();
    setState(State::Relaying);
}

void Ctr2UsbRelay::sessionDraining(quint64 generation)
{
    if (generation == m_generation) {
        setState(State::Closing);
    }
}

void Ctr2UsbRelay::sessionEnded(quint64 generation, const QString& message, bool error,
                                bool sendClosed)
{
    if (generation != m_generation || !m_session) {
        return;
    }
    m_lastSessionStats = m_session->m_finalStats;
    Session* session = m_session;
    m_session = nullptr;
    QObject::disconnect(session, nullptr, this, nullptr);
    session->deleteLater();
    m_awaitingHello = true;
    if (sendClosed) {
        sendControl(MessageType::Closed);
    }
    if (error) {
        qCWarning(lcDevices) << "CTR2 USB:" << message;
        setLastError(message);
    } else {
        qCInfo(lcDevices) << "CTR2 USB:" << message;
    }
    if (m_port) {
        setState(State::Listening);
    }
    emit endpointsChanged();
    emit statsChanged();
}

void Ctr2UsbRelay::endSession()
{
    if (!m_session) {
        return;
    }
    // A session that did not end through its own callbacks is cut off here.
    Session* session = m_session;
    m_session = nullptr;
    ++m_generation;
    QObject::disconnect(session, nullptr, this, nullptr);
    session->deleteLater();
}

QString Ctr2UsbRelay::deviceDescription() const
{
    return m_port ? m_port->description() : QString();
}

QString Ctr2UsbRelay::radioDescription() const
{
    return m_radioAddress.isNull() ? QString() : endpointText(m_radioAddress, m_radioPort);
}

Ctr2UsbRelay::Stats Ctr2UsbRelay::stats() const
{
    return m_session ? m_session->stats() : m_lastSessionStats;
}

void Ctr2UsbRelay::setState(State state)
{
    if (m_state == state) {
        return;
    }
    m_state = state;
    emit stateChanged(state);
}

void Ctr2UsbRelay::setLastError(const QString& message)
{
    if (m_lastError == message) {
        return;
    }
    m_lastError = message;
    emit lastErrorChanged(message);
}

} // namespace AetherSDR
