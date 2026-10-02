#include "Kpa500Connection.h"

#include <QLoggingCategory>

Q_LOGGING_CATEGORY(lcKpa500, "aethersdr.kpa500")

namespace AetherSDR {

Kpa500Connection::Kpa500Connection(QObject* parent)
    : QObject(parent)
{
    m_reconnectTimer.setSingleShot(true);
    m_reconnectTimer.setInterval(kReconnectMs);
    connect(&m_reconnectTimer, &QTimer::timeout, this, [this] {
#ifdef HAVE_SERIALPORT
        if (!m_lastPortName.isEmpty())
            connectSerial(m_lastPortName, m_lastBaudRate);
#endif
    });

    m_fastPollTimer.setInterval(kFastPollMs);
    connect(&m_fastPollTimer, &QTimer::timeout, this, &Kpa500Connection::fastPollTick);

    m_slowPollTimer.setInterval(kSlowPollMs);
    connect(&m_slowPollTimer, &QTimer::timeout, this, &Kpa500Connection::slowPollTick);

    m_parser.setCallback([this](const QString& cmd, const QString& arg) {
        m_statusSeenSinceFastTick = true;
        if (!m_responding) {
            m_silentTicks = 0;
            setResponding(true);
        }
        if (Kpa500::applyMessage(cmd, arg, m_status))
            emit statusUpdated(m_status);
    });
}

QString Kpa500Connection::description() const
{
    if (m_lastPortName.isEmpty())
        return {};
    return QStringLiteral("%1 @ %2").arg(m_lastPortName).arg(m_lastBaudRate);
}

#ifdef HAVE_SERIALPORT

void Kpa500Connection::connectSerial(const QString& portName, int baudRate)
{
    if (m_connected)
        return;

    m_lastPortName  = portName;
    m_lastBaudRate  = baudRate;
    m_deliberateDisconnect = false;

    qCDebug(lcKpa500) << "Kpa500Connection: opening" << portName << "@" << baudRate;

    m_serial.setPortName(portName);
    m_serial.setBaudRate(baudRate);
    m_serial.setDataBits(QSerialPort::Data8);
    m_serial.setParity(QSerialPort::NoParity);
    m_serial.setStopBits(QSerialPort::OneStop);
    m_serial.setFlowControl(QSerialPort::NoFlowControl);

    if (!m_serial.open(QIODevice::ReadWrite)) {
        const QString err = m_serial.errorString();
        qCWarning(lcKpa500) << "Kpa500Connection: open failed:" << err;
        emit connectionFailed(err);
        armReconnect();
        return;
    }

    onTransportUp();
}

void Kpa500Connection::onReadyRead()
{
    m_parser.feed(m_serial.readAll());
}

void Kpa500Connection::onSerialError(QSerialPort::SerialPortError error)
{
    if (error == QSerialPort::NoError)
        return;
    if (!m_connected)
        return;
    qCWarning(lcKpa500) << "Kpa500Connection: serial error:" << error << m_serial.errorString();
    onTransportDown(m_serial.errorString());
}

#endif  // HAVE_SERIALPORT

void Kpa500Connection::disconnect()
{
    m_deliberateDisconnect = true;
    m_reconnectTimer.stop();
    onTransportDown();
}

void Kpa500Connection::setOperate(bool on)
{
    sendRaw(on ? Kpa500::buildOperate() : Kpa500::buildStandby());
}

void Kpa500Connection::clearFault()
{
    sendRaw(Kpa500::buildClearFault());
}

void Kpa500Connection::setFanSpeed(int n)
{
    sendRaw(Kpa500::buildSetFanSpeed(n));
}

// ── Private ───────────────────────────────────────────────────────────────────

void Kpa500Connection::onTransportUp()
{
    m_connected = true;
    m_status    = {};
    m_parser.reset();
    m_slowIndex       = 0;
    m_fastIndex       = 0;
    m_connectCmdIndex = 0;
    m_connectCmdsDone = false;
    m_silentTicks     = 0;
    m_statusSeenSinceFastTick = false;

#ifdef HAVE_SERIALPORT
    connect(&m_serial, &QSerialPort::readyRead,
            this, &Kpa500Connection::onReadyRead);
    connect(&m_serial, &QSerialPort::errorOccurred,
            this, &Kpa500Connection::onSerialError);
#endif

    // Send a null command first to verify communication before starting polls.
    sendRaw(Kpa500::buildNullCommand());

    m_fastPollTimer.start();
    m_slowPollTimer.start();

    qCDebug(lcKpa500) << "Kpa500Connection: connected to" << description();
    emit connected();
}

void Kpa500Connection::onTransportDown(const QString& reason)
{
#ifdef HAVE_SERIALPORT
    if (!m_connected && !m_serial.isOpen())
        return;
#else
    if (!m_connected)
        return;
#endif

    m_fastPollTimer.stop();
    m_slowPollTimer.stop();
    m_parser.reset();

#ifdef HAVE_SERIALPORT
    m_serial.disconnect(this);
    if (m_serial.isOpen())
        m_serial.close();
#endif

    if (m_responding)
        setResponding(false);

    m_connected = false;
    if (!reason.isEmpty())
        qCDebug(lcKpa500) << "Kpa500Connection: disconnected:" << reason;
    else
        qCDebug(lcKpa500) << "Kpa500Connection: disconnected";

    emit disconnected();

    if (m_autoReconnect && !m_deliberateDisconnect)
        armReconnect();
}

void Kpa500Connection::fastPollTick()
{
    if (!m_connected)
        return;

    // Send remaining one-shot connect commands first (one per fast tick).
    if (!m_connectCmdsDone) {
        const QStringList& cmds = Kpa500::connectCommands();
        if (m_connectCmdIndex < cmds.size()) {
            sendRaw(Kpa500::buildQuery(cmds[m_connectCmdIndex].toLatin1().constData()));
            ++m_connectCmdIndex;
            return;
        }
        m_connectCmdsDone = true;
    }

    // Poll-silence tracking.
    if (m_statusSeenSinceFastTick) {
        m_silentTicks = 0;
        m_statusSeenSinceFastTick = false;
    } else {
        ++m_silentTicks;
        if (m_responding && m_silentTicks >= kSilentTickLimit)
            setResponding(false);
    }

    // Rotate through fast-poll commands.
    const QStringList& cmds = Kpa500::fastPollCommands();
    if (!cmds.isEmpty()) {
        m_fastIndex = (m_fastIndex + 1) % cmds.size();
        sendRaw(Kpa500::buildQuery(cmds[m_fastIndex].toLatin1().constData()));
    }
}

void Kpa500Connection::slowPollTick()
{
    if (!m_connected || !m_connectCmdsDone)
        return;

    const QStringList& cmds = Kpa500::slowPollCommands();
    if (cmds.isEmpty())
        return;

    sendRaw(Kpa500::buildQuery(cmds[m_slowIndex].toLatin1().constData()));
    m_slowIndex = (m_slowIndex + 1) % cmds.size();
}

void Kpa500Connection::sendRaw(const QByteArray& bytes)
{
#ifdef HAVE_SERIALPORT
    if (m_connected && m_serial.isOpen())
        m_serial.write(bytes);
#else
    Q_UNUSED(bytes)
#endif
}

void Kpa500Connection::armReconnect()
{
    if (!m_deliberateDisconnect && m_autoReconnect)
        m_reconnectTimer.start();
}

void Kpa500Connection::setResponding(bool responding)
{
    if (m_responding == responding)
        return;
    m_responding = responding;
    emit respondingChanged(responding);
}

}  // namespace AetherSDR
